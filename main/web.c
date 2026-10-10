/*
 * Web UI — see web.h.
 *
 * Pages: /          price table (public)
 *        /relay     relay schedule & price limits
 *        /settings  time, diagnostics, password, language
 *        /mqtt      MQTT broker
 *        /wifi      network settings (also the setup page in AP mode)
 *        /ota       firmware upload
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "lwip/inet.h"
#include "mbedtls/base64.h"
#include "display.h"
#include "settings.h"
#include "wifi.h"
#include "mqtt_pub.h"
#include "ui.h"
#include "app.h"
#include "web.h"

static const char *TAG = "web";

/* Picks between English and Estonian at runtime */
#define T(en, et) (g_cfg.lang == LANG_ET ? (et) : (en))

/* First line of a POST result page: confirmation, or a warning when the
 * settings were applied but could not be stored in flash. */
static const char *saved_msg(esp_err_t save_err)
{
    if (save_err == ESP_OK)
        return T("<p>&#x2714; Saved &amp; applied.</p>",
                 "<p>&#x2714; Salvestatud &amp; rakendatud.</p>");
    return T("<p style='color:#b71c1c'>&#x26A0; Applied, but saving to flash failed"
             " &mdash; these settings will be lost on reboot.</p>",
             "<p style='color:#b71c1c'>&#x26A0; Rakendatud, kuid m&auml;llu salvestamine"
             " eba&otilde;nnestus &mdash; seaded kaovad taask&auml;ivitusel.</p>");
}

/* ── URL / form helpers ──────────────────────────────────────────────────── */

static void url_decode(char *dst, const char *src, size_t maxlen)
{
    size_t i = 0;
    while (*src && i + 1 < maxlen) {
        if (*src == '+') {
            dst[i++] = ' '; src++;
        } else if (*src == '%'
                   && isxdigit((unsigned char)src[1])
                   && isxdigit((unsigned char)src[2])) {
            int hi = isdigit((unsigned char)src[1]) ? src[1]-'0' : tolower((unsigned char)src[1])-'a'+10;
            int lo = isdigit((unsigned char)src[2]) ? src[2]-'0' : tolower((unsigned char)src[2])-'a'+10;
            dst[i++] = (char)(hi * 16 + lo);
            src += 3;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static void form_field(const char *body, const char *key,
                       char *out, size_t outlen)
{
    char needle[72];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(needle);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char raw[CRED_LEN * 3] = {0};
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    url_decode(out, raw, outlen);
}

static void html_escape(const char *in, char *out, size_t outlen)
{
    size_t i = 0;
    while (*in && i + 7 < outlen) {
        if      (*in == '<') { memcpy(out+i, "&lt;",   4); i += 4; }
        else if (*in == '>') { memcpy(out+i, "&gt;",   4); i += 4; }
        else if (*in == '&') { memcpy(out+i, "&amp;",  5); i += 5; }
        else if (*in == '"') { memcpy(out+i, "&quot;", 6); i += 6; }
        else if (*in == '\'') { memcpy(out+i, "&#39;",  5); i += 5; }
        else                 { out[i++] = *in; }
        in++;
    }
    out[i] = '\0';
}


/* ── Web access control ──────────────────────────────────────────────────── */

/* Constant-time compare so the password can't be guessed byte by byte */
static bool ct_equal(const char *a, const char *b, size_t n)
{
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/* HTTP Basic auth (user AUTH_USER, password g_cfg.dev_pass). Sends 401 and
 * returns false when the request is not authorized. In AP mode the setup
 * network itself is WPA2-protected with the same password, so no login. */
static bool auth_ok(httpd_req_t *req)
{
    if (g_ap_mode) return true;

    bool had_hdr = false;
    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0) {
        had_hdr = true;
        unsigned char dec[96];
        size_t olen = 0;
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &olen,
                                  (const unsigned char *)hdr + 6, strlen(hdr + 6)) == 0) {
            char expect[sizeof(AUTH_USER) + DEV_PASS_MAX + 1];
            snprintf(expect, sizeof(expect), "%s:%s", AUTH_USER, g_cfg.dev_pass);
            if (olen == strlen(expect) && ct_equal((const char *)dec, expect, olen))
                return true;
        }
    }
    if (had_hdr) {
        ESP_LOGW(TAG, "Web login failed");
        vTaskDelay(pdMS_TO_TICKS(1000));   /* slow down password guessing */
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"EleRelay\", charset=\"UTF-8\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Authentication required");
    return false;
}

/* CSRF protection for POST handlers. Browsers resend Basic-auth credentials
 * automatically, so a form on another website could otherwise post here.
 * Browsers always send Origin (or at least Referer) on such requests; it
 * must point at this device's Host. Requests with neither header (curl,
 * scripts) are allowed — they still need the password. */
static bool origin_ok(httpd_req_t *req)
{
    const char *name = NULL;
    if (httpd_req_get_hdr_value_len(req, "Origin") > 0)       name = "Origin";
    else if (httpd_req_get_hdr_value_len(req, "Referer") > 0) name = "Referer";
    if (!name) return true;

    char host[64] = "", src[256] = "";
    bool ok = httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK &&
              host[0] &&
              httpd_req_get_hdr_value_str(req, name, src, sizeof(src)) == ESP_OK;
    if (ok) {
        /* src = scheme://host[:port][/path...] */
        const char *p = strstr(src, "://");
        p = p ? p + 3 : src;
        size_t hl = strlen(host);
        ok = strncmp(p, host, hl) == 0 && (p[hl] == '\0' || p[hl] == '/');
    }
    if (!ok) {
        ESP_LOGW(TAG, "Rejected cross-site POST (%s: %s, Host: %s)", name, src, host);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-site request rejected");
    }
    return ok;
}

/* Guards for handlers: GET pages need login; POSTs need login + same origin */
#define REQUIRE_AUTH(req)       do { if (!auth_ok(req)) return ESP_OK; } while (0)
#define REQUIRE_AUTH_POST(req)  do { if (!origin_ok(req) || !auth_ok(req)) return ESP_FAIL; } while (0)


/* ── Common CSS ──────────────────────────────────────────────────────────── */

static const char *PAGE_CSS =
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:680px;margin:30px auto;"
         "padding:0 16px;color:#222}"
    "h1{font-size:1.4rem;margin-bottom:4px}"
    "h2{font-size:1.1rem;margin:22px 0 4px;padding-top:16px;"
        "border-top:1px solid #ddd;color:#444}"
    "nav{margin-bottom:14px;font-size:.88rem}"
    "nav a{color:#1565c0;margin-right:12px}"
    ".status{display:inline-block;padding:3px 11px;border-radius:20px;"
            "font-weight:600}"
    ".on{background:#c8e6c9;color:#1b5e20}"
    ".off{background:#ffcdd2;color:#b71c1c}"
    "table{width:100%;border-collapse:collapse;margin-top:16px;font-size:.9rem}"
    "th,td{padding:7px 10px;border:1px solid #ddd;text-align:center}"
    "th{background:#f5f5f5;font-weight:600}"
    "tr.cheap td{background:#e8f5e9}"
    "tr.exp   td{background:#ffebee}"
    "tr.cur   td{outline:2px solid #333;outline-offset:-1px;font-weight:700}"
    "tr.hg td:first-child{border-left:2px solid #888}"
    "tr.hg td:last-child{border-right:2px solid #888}"
    "tr.hs td{border-top:2px solid #888}"
    "tr.he td{border-bottom:2px solid #888}"
    "tr.win-sep td{background:#e8eaf6;color:#283593;font-weight:600;"
                  "font-size:.82rem;text-align:left;border-top:3px solid #5c6bc0;"
                  "border-bottom:2px solid #9fa8da}"
    "td.ron{color:#2e7d32;font-weight:600}"
    "td.roff{color:#c62828;font-weight:600}"
    ".meta{color:#666;font-size:.82rem;margin:3px 0}"
    "label{display:block;margin-top:14px;font-size:.9rem;color:#555}"
    "select,input[type=number],input[type=text],input[type=password]{"
        "width:100%;box-sizing:border-box;padding:7px;margin-top:4px;"
        "border:1px solid #ccc;border-radius:6px;font-size:1rem}"
    "select{background:#fff}"
    ".chk{display:flex;align-items:center;gap:8px;margin-top:14px;"
          "font-size:.9rem;color:#555}"
    ".chk input{width:auto;margin:0}"
    "button{margin-top:20px;width:100%;padding:10px;background:#1565c0;"
            "color:#fff;border:none;border-radius:6px;font-size:1rem;cursor:pointer}"
    "button:hover{background:#0d47a1}"
    ".note{color:#666;font-size:.82rem;margin-top:8px}"
    "progress{width:100%;height:22px;margin-top:12px;border-radius:4px}"
    ".ota-status{margin-top:10px;font-size:.95rem;font-weight:600}"
    "</style>";

static void send_page_head(httpd_req_t *req, const char *title)
{
    char buf[512];
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html lang='%s'><head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<link rel='icon' href=\"data:image/svg+xml,"
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'>"
        "<text y='.9em' font-size='90'>&#x26A1;</text></svg>\">",
        T("en", "et"));
    httpd_resp_sendstr_chunk(req, buf);
    snprintf(buf, sizeof(buf), "<title>%s</title>", title);
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req, PAGE_CSS);
    snprintf(buf, sizeof(buf),
        "</head><body><nav>"
        "<a href='/'>&#x26A1; %s</a>"
        "<a href='/relay'>&#x1F4CA; %s</a>"
        "<a href='/settings'>&#x2699;&#xFE0F; %s</a>"
        "<a href='/mqtt'>&#x1F4E1; MQTT</a>"
        "<a href='/wifi'>&#x1F4F6; %s</a>"
        "<a href='/ota'>&#x1F4E6; OTA</a>"
        "</nav>",
        T("Prices", "Hinnad"),
        T("Relay Settings", "Relee Seaded"),
        T("Settings", "Seaded"),
        T("Network", "V&otilde;rk"));
    httpd_resp_sendstr_chunk(req, buf);
}

/* ── /relay GET ──────────────────────────────────────────────────────────── */

static esp_err_t relay_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    send_page_head(req, T("EleRelay \xe2\x80\x94 Relay Settings", "EleRelay \xe2\x80\x94 Relee Seaded"));
    char chunk[512];
    snprintf(chunk, sizeof(chunk), "<h1>&#x1F4CA; %s</h1>"
        "<form method='POST' action='/relay'>", T("Relay Settings", "Relee Seaded"));
    httpd_resp_sendstr_chunk(req, chunk);

    /* ── Relay / price settings ── */
    snprintf(chunk, sizeof(chunk), "<h2>%s</h2>", T("Relay &amp; Price", "Relee &amp; Hind"));
    httpd_resp_sendstr_chunk(req, chunk);

    /* Hours window dropdown */
    snprintf(chunk, sizeof(chunk), "<label>%s<select name='window'>",
             T("Hours window", "Ajaaken"));
    httpd_resp_sendstr_chunk(req, chunk);
    for (int i = 2; i <= 24; i++) {
        snprintf(chunk, sizeof(chunk), "<option value='%d'%s>%d %s</option>",
                 i, i == g_cfg.hours_window ? " selected" : "", i, T("hours", "tundi"));
        httpd_resp_sendstr_chunk(req, chunk);
    }
    httpd_resp_sendstr_chunk(req, "</select></label>");

    /* Cheap hours dropdown */
    snprintf(chunk, sizeof(chunk), "<label>%s<select name='cheap'>",
             T("Cheap hours", "Odavad tunnid"));
    httpd_resp_sendstr_chunk(req, chunk);
    for (int i = 1; i < g_cfg.hours_window; i++) {
        snprintf(chunk, sizeof(chunk), "<option value='%d'%s>%d %s</option>",
                 i, i == g_cfg.cheap_hours ? " selected" : "", i, T("hours", "tundi"));
        httpd_resp_sendstr_chunk(req, chunk);
    }
    httpd_resp_sendstr_chunk(req, "</select></label>");

    /* Minimum continuous run minutes */
    {
        char buf[600];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='number' name='min_run' min='0' max='480' value='%d'>"
            "</label>"
            "<p class='note'>%s</p>",
            T("Minimum continuous run (minutes, 0 = disabled)",
              "Minimaalne j&auml;rjestikune k&auml;itusaeg (minutit, 0 = v&auml;lja l&uuml;litatud)"),
            g_cfg.min_run_minutes,
            T("Once the relay turns ON it stays ON for at least this many minutes."
              " Useful for devices that need a full run cycle (e.g. 60 min = 1 hour).",
              "Kui relee l&auml;heb SISSE, j&auml;&auml;b see sisselülitatuks v&auml;hemalt nii kauaks."
              " Kasulik seadmetele, mis vajavad t&auml;ielikku t&ouml;&ouml;tsüklit (nt 60 min = 1 tund)."));
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* Always ON / OFF overrides */
    snprintf(chunk, sizeof(chunk), "<h2>%s</h2>",
        T("Override: Always ON / Always OFF", "Limiidid: Alati SEES / V&auml;ljas"));
    httpd_resp_sendstr_chunk(req, chunk);

    {
        char buf[640];
        snprintf(buf, sizeof(buf),
            "<div class='chk'>"
            "<input type='checkbox' name='aon_en' id='aon_en'%s>"
            "<label for='aon_en' style='margin:0;color:#222'>%s</label>"
            "</div>"
            "<input type='text' inputmode='decimal' name='aon_lim'"
            " style='width:100%%;box-sizing:border-box;padding:7px;margin-top:4px;"
            "border:1px solid #ccc;border-radius:6px;font-size:1rem'"
            " value='%.1f'>",
            g_cfg.always_on_en ? " checked" : "",
            T("Always ON when price is at or below (c/kWh):",
              "Alati SEES kui hind on kuni (s/kWh):"),
            g_cfg.always_on_limit / 10.0f);
        httpd_resp_sendstr_chunk(req, buf);
    }
    snprintf(chunk, sizeof(chunk), "<p class='note'>%s</p>",
        T("Force relay ON whenever price &le; this value, ignoring the schedule.",
          "Sunnib relee SISSE kui hind &le; v&auml;&auml;rtus, ignoreerides graafiku."));
    httpd_resp_sendstr_chunk(req, chunk);

    {
        char buf[640];
        snprintf(buf, sizeof(buf),
            "<div class='chk'>"
            "<input type='checkbox' name='aoff_en' id='aoff_en'%s>"
            "<label for='aoff_en' style='margin:0;color:#222'>%s</label>"
            "</div>"
            "<input type='text' inputmode='decimal' name='aoff_lim'"
            " style='width:100%%;box-sizing:border-box;padding:7px;margin-top:4px;"
            "border:1px solid #ccc;border-radius:6px;font-size:1rem'"
            " value='%.1f'>",
            g_cfg.always_off_en ? " checked" : "",
            T("Always OFF when price is at or above (c/kWh):",
              "Alati V&auml;ljas kui hind on v&auml;hemalt (s/kWh):"),
            g_cfg.always_off_limit / 10.0f);
        httpd_resp_sendstr_chunk(req, buf);
    }
    snprintf(chunk, sizeof(chunk), "<p class='note'>%s</p>",
        T("Force relay OFF whenever price &ge; this value. Takes priority over Always ON and min-run.",
          "Sunnib relee V&auml;LJA kui hind &ge; v&auml;&auml;rtus. Prioriteet Always ON ja min-k&auml;ituse ees."));
    httpd_resp_sendstr_chunk(req, chunk);

    /* Relay inverted checkbox */
    {
        char buf[320];
        snprintf(buf, sizeof(buf),
            "<div class='chk'>"
            "<input type='checkbox' name='inv' id='inv'%s>"
            "<label for='inv' style='margin:0;color:#222'>%s</label>"
            "</div>",
            g_cfg.relay_inv ? " checked" : "",
            T("Inverted &mdash; ON during expensive hours",
              "P&ouml;&ouml;ratud &mdash; SEES kallite tundide ajal"));
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* Fetch hour dropdown */
    snprintf(chunk, sizeof(chunk), "<label>%s<select name='fetch_h'>",
             T("Fetch prices at (daily)", "Laadi hinnad (iga p&auml;ev)"));
    httpd_resp_sendstr_chunk(req, chunk);
    for (int h = 0; h < 24; h++) {
        snprintf(chunk, sizeof(chunk), "<option value='%d'%s>%02d:00</option>",
                 h, h == g_cfg.fetch_hour ? " selected" : "", h);
        httpd_resp_sendstr_chunk(req, chunk);
    }
    httpd_resp_sendstr_chunk(req, "</select></label>");
    snprintf(chunk, sizeof(chunk), "<p class='note'>%s</p>",
        T("Elering publishes next-day prices around 14:00 EET."
          " Fetching at 23:00 ensures a full set of tomorrow&rsquo;s prices.",
          "Elering avaldab j&auml;rgmise p&auml;eva hinnad umbes 14:00 EET."
          " Laadimine kell 23:00 tagab t&auml;ieliku hinnakomplekti."));
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
        "<button type='submit'>%s</button>"
        "</form></body></html>",
        T("Save settings", "Salvesta seaded"));
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /settings GET ───────────────────────────────────────────────────────── */

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    send_page_head(req, T("EleRelay \xe2\x80\x94 Settings", "EleRelay \xe2\x80\x94 Seaded"));
    char chunk[512];
    snprintf(chunk, sizeof(chunk), "<h1>&#x2699;&#xFE0F; %s</h1>"
        "<form method='POST' action='/settings'>", T("Settings", "Seaded"));
    httpd_resp_sendstr_chunk(req, chunk);

    /* ── Time settings ── */
    snprintf(chunk, sizeof(chunk), "<h2>%s</h2>", T("Time", "Aeg"));
    httpd_resp_sendstr_chunk(req, chunk);

    /* NTP server */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(g_cfg.ntp_server, safe, sizeof(safe));
        char buf[512];
        snprintf(buf, sizeof(buf),
            "<label>NTP server"
            "<input type='text' name='ntp_server' maxlength='%d' value='%s'>"
            "</label>", STR_LEN - 1, safe);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* Timezone POSIX TZ string */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(g_cfg.tz_str, safe, sizeof(safe));
        char buf[768];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='text' name='tz_str' maxlength='%d' value='%s'>"
            "</label>"
            "<p class='note'>%s: "
            "<code>EET-2EEST,M3.5.0/3,M10.5.0/4</code> (Estonia), "
            "<code>UTC0</code>, "
            "<code>CET-1CEST,M3.5.0,M10.5.0/3</code> (Central Europe)</p>",
            T("Timezone (POSIX TZ string)", "Ajev&ouml;&ouml;nd (POSIX)"),
            STR_LEN - 1, safe,
            T("Examples", "N&auml;ited"));
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* ── Diagnostics ── */
    snprintf(chunk, sizeof(chunk), "<h2>%s</h2>", T("Diagnostics", "Diagnostika"));
    httpd_resp_sendstr_chunk(req, chunk);
    {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='number' name='inet_chk' min='0' max='60' value='%d'>"
            "</label>"
            "<p class='note'>%s</p>",
            T("Internet check interval (minutes, 0 = disabled)",
              "Interneti kontrolli intervall (minutit, 0 = v&auml;lja l&uuml;litatud)"),
            g_cfg.inet_check_min,
            T("Periodically checks internet connectivity. Shows a warning on the LCD if unreachable.",
              "Kontrollib perioodiliselt interneti&uuml;hendust. Kuvab LCD-l hoiatuse, kui &uuml;hendus puudub."));
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* ── Device password ── */
    snprintf(chunk, sizeof(chunk), "<h2>%s</h2>", T("Password", "Parool"));
    httpd_resp_sendstr_chunk(req, chunk);
    {
        char buf[768];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='password' name='dev_pass' minlength='%d' maxlength='%d'"
            " autocomplete='new-password' placeholder='%s'>"
            "</label>"
            "<p class='note'>%s</p>",
            T("New device password", "Uus seadme parool"),
            DEV_PASS_MIN, DEV_PASS_MAX,
            T("Leave blank to keep the current password", "J&auml;ta t&uuml;hjaks, et parool s&auml;iliks"),
            T("Web login user is <b>" AUTH_USER "</b>. The same password protects the"
              " <code>" AP_SSID "</code> setup WiFi. 8&ndash;32 characters."
              " The current password is shown on the LCD info page (tap screen, page 4/4).",
              "Veebi kasutajanimi on <b>" AUTH_USER "</b>. Sama parool kaitseb"
              " <code>" AP_SSID "</code> seadistusv&otilde;rku. 8&ndash;32 m&auml;rki."
              " Praegune parool on n&auml;ha LCD infolehel (puuduta ekraani, leht 4/4)."));
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* ── Language / Keel ── */
    httpd_resp_sendstr_chunk(req, "<h2>Keel / Language</h2>");
    snprintf(chunk, sizeof(chunk),
        "<label>Keel / Language"
        "<select name='lang'>"
        "<option value='0'%s>English</option>"
        "<option value='1'%s>Eesti</option>"
        "</select></label>",
        g_cfg.lang == LANG_EN ? " selected" : "",
        g_cfg.lang == LANG_ET ? " selected" : "");
    httpd_resp_sendstr_chunk(req, chunk);

    snprintf(chunk, sizeof(chunk),
        "<button type='submit'>%s</button>"
        "</form></body></html>",
        T("Save settings", "Salvesta seaded"));
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /relay POST ─────────────────────────────────────────────────────────── */

static esp_err_t relay_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH_POST(req);
    char body[1024] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[len] = '\0';

    char val[STR_LEN];

    form_field(body, "window",   val, sizeof(val));
    int window = clamp(atoi(val), 2, 24);

    form_field(body, "cheap",    val, sizeof(val));
    int cheap = clamp(atoi(val), 1, window - 1 < 1 ? 1 : window - 1);

    /* checkbox: present = checked, absent = unchecked */
    bool inv = (strstr(body, "inv=on") || strstr(body, "inv=1"));

    form_field(body, "fetch_h",  val, sizeof(val));
    int fetch_h = clamp(atoi(val), 0, 23);

    form_field(body, "min_run", val, sizeof(val));
    int min_run = clamp(atoi(val), 0, 480);

    bool aon_en = (strstr(body, "aon_en=on") || strstr(body, "aon_en=1"));
    form_field(body, "aon_lim", val, sizeof(val));
    for (char *p = val; *p; p++) if (*p == ',') *p = '.';  /* locale-safe */
    int aon_lim = clamp((int)lroundf(atof(val) * 10.0f), PRICE_LIM_MIN, PRICE_LIM_MAX);

    bool aoff_en = (strstr(body, "aoff_en=on") || strstr(body, "aoff_en=1"));
    form_field(body, "aoff_lim", val, sizeof(val));
    for (char *p = val; *p; p++) if (*p == ',') *p = '.';  /* locale-safe */
    int aoff_lim = clamp((int)lroundf(atof(val) * 10.0f), PRICE_LIM_MIN, PRICE_LIM_MAX);

    /* Apply immediately */
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_cfg.hours_window = window;
    g_cfg.cheap_hours  = cheap;
    g_cfg.relay_inv    = inv;
    g_cfg.fetch_hour   = fetch_h;
    g_cfg.min_run_minutes  = min_run;
    g_cfg.always_on_en     = aon_en;
    g_cfg.always_on_limit  = aon_lim;
    g_cfg.always_off_en    = aoff_en;
    g_cfg.always_off_limit = aoff_lim;
    app_recompute_cheap();   /* recompute with new cheap count */
    xSemaphoreGive(g_state_mutex);

    update_relay();          /* reflect new inversion + cheap hours instantly */
    esp_err_t save_err = settings_save();
    update_display();        /* reflect new settings on LCD immediately */

    ESP_LOGI(TAG, "Relay settings updated: window=%d cheap=%d inv=%d fetch_h=%d "
             "min_run=%d aon=%d@%d aoff=%d@%d",
             window, cheap, inv, fetch_h, min_run,
             aon_en, aon_lim, aoff_en, aoff_lim);

    send_page_head(req, T("EleRelay \xe2\x80\x94 Relay Settings", "EleRelay \xe2\x80\x94 Relee Seaded"));
    char chunk[1280];
    snprintf(chunk, sizeof(chunk),
        "<h1>%s</h1>"
        "%s"
        "<ul style='font-size:.9rem;line-height:1.8'>"
        "<li>%s <b>%d h</b></li>"
        "<li>%s <b>%d</b></li>"
        "<li>%s <b>%d min</b></li>"
        "<li>%s <b>%s</b></li>"
        "<li>%s <b>%02d:00</b></li>"
        "<li>%s <b>%s</b> (%.1f c/kWh)</li>"
        "<li>%s <b>%s</b> (%.1f c/kWh)</li>"
        "</ul>"
        "<p><a href='/relay'>%s</a>"
        " &nbsp; <a href='/'>%s</a></p>"
        "</body></html>",
        T("Relay Settings", "Relee Seaded"),
        saved_msg(save_err),
        T("Window:", "Aken:"), window,
        T("Cheap hours:", "Odavad tunnid:"), cheap,
        T("Min. continuous run:", "Min. j&auml;rjestikune k&auml;itus:"), min_run,
        T("Relay inverted:", "Relee p&ouml;&ouml;ratud:"), inv ? T("yes","jah") : T("no","ei"),
        T("Fetch at:", "Laadimine:"), fetch_h,
        T("Always ON &le;:", "Alati SEES &le;:"),
        aon_en ? T("on","sees") : T("off","v&auml;ljas"), aon_lim / 10.0f,
        T("Always OFF &ge;:", "Alati V&auml;ljas &ge;:"),
        aoff_en ? T("on","sees") : T("off","v&auml;ljas"), aoff_lim / 10.0f,
        T("Back to relay settings", "Tagasi relee seadetesse"),
        T("Price table", "Hinnad"));
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /settings POST ──────────────────────────────────────────────────────── */

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH_POST(req);
    char body[1024] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[len] = '\0';

    char val[STR_LEN];

    /* Time settings */
    char ntp_server[STR_LEN] = {0};
    form_field(body, "ntp_server", ntp_server, sizeof(ntp_server));
    if (ntp_server[0] == '\0') strlcpy(ntp_server, "pool.ntp.org", sizeof(ntp_server));

    char tz_str[STR_LEN] = {0};
    form_field(body, "tz_str", tz_str, sizeof(tz_str));
    if (tz_str[0] == '\0') strlcpy(tz_str, "UTC0", sizeof(tz_str));

    form_field(body, "lang", val, sizeof(val));
    int new_lang = clamp(atoi(val), 0, 1);

    form_field(body, "inet_chk", val, sizeof(val));
    int inet_chk = clamp(atoi(val), 0, 60);

    /* Optional new device password (blank = keep): 8-32 printable ASCII
     * characters (WPA2-compatible, short enough to show on the LCD). */
    char dev_pass[DEV_PASS_MAX + 2] = {0};
    form_field(body, "dev_pass", dev_pass, sizeof(dev_pass));
    size_t dp_len = strlen(dev_pass);
    if (dp_len > 0) {
        bool valid = dp_len >= DEV_PASS_MIN && dp_len <= DEV_PASS_MAX;
        for (size_t k = 0; valid && k < dp_len; k++)
            if (dev_pass[k] < 0x20 || dev_pass[k] > 0x7E) valid = false;
        if (!valid) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "Password must be 8-32 printable ASCII characters");
            return ESP_FAIL;
        }
    }

    /* Apply immediately */
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    strlcpy(g_cfg.ntp_server,       ntp_server,  sizeof(g_cfg.ntp_server));
    strlcpy(g_cfg.tz_str,           tz_str,      sizeof(g_cfg.tz_str));
    g_cfg.lang = new_lang;
    g_cfg.inet_check_min = inet_chk;
    if (dp_len > 0) strlcpy(g_cfg.dev_pass, dev_pass, sizeof(g_cfg.dev_pass));
    xSemaphoreGive(g_state_mutex);
    esp_err_t save_err = ESP_OK;
    if (dp_len > 0) {
        save_err = dev_pass_save();
        ESP_LOGI(TAG, "Device password changed");
    }
    display_set_lang(new_lang);

    /* Apply timezone immediately */
    setenv("TZ", g_cfg.tz_str, 1);
    tzset();

    esp_err_t err2 = settings_save();
    if (save_err == ESP_OK) save_err = err2;
    update_display();        /* reflect new language on LCD immediately */

    ESP_LOGI(TAG, "Settings updated: ntp=%s tz=%s lang=%d inet_chk=%d",
             ntp_server, tz_str, new_lang, inet_chk);

    send_page_head(req, T("EleRelay \xe2\x80\x94 Settings", "EleRelay \xe2\x80\x94 Seaded"));
    char safe_ntp[STR_LEN * 6], safe_tz[STR_LEN * 6];
    html_escape(ntp_server, safe_ntp, sizeof(safe_ntp));
    html_escape(tz_str,     safe_tz,  sizeof(safe_tz));
    char chunk[1280];
    snprintf(chunk, sizeof(chunk),
        "<h1>%s</h1>"
        "%s"
        "<ul style='font-size:.9rem;line-height:1.8'>"
        "<li>NTP: <b>%s</b></li>"
        "<li>TZ: <b>%s</b></li>"
        "%s"
        "</ul>"
        "<p><a href='/settings'>%s</a>"
        " &nbsp; <a href='/'>%s</a></p>"
        "</body></html>",
        T("Settings", "Seaded"),
        saved_msg(save_err),
        safe_ntp, safe_tz,
        dp_len > 0 ? T("<li><b>Password changed</b> &mdash; log in again with the new password.</li>",
                       "<li><b>Parool muudetud</b> &mdash; logi uue parooliga uuesti sisse.</li>") : "",
        T("Back to settings", "Tagasi seadetesse"),
        T("Price table", "Hinnad"));
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /mqtt GET ───────────────────────────────────────────────────────────── */

static esp_err_t mqtt_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    send_page_head(req, "EleRelay \xe2\x80\x94 MQTT");
    char chunk[512];
    snprintf(chunk, sizeof(chunk), "<h1>&#x1F4E1; MQTT</h1>"
        "<form method='POST' action='/mqtt'>");
    httpd_resp_sendstr_chunk(req, chunk);

    /* MQTT enabled checkbox */
    {
        char buf[320];
        snprintf(buf, sizeof(buf),
            "<div class='chk'>"
            "<input type='checkbox' name='mqtt_en' id='mqtt_en'%s>"
            "<label for='mqtt_en' style='margin:0;color:#222'>%s</label>"
            "</div>",
            g_cfg.mqtt_enabled ? " checked" : "",
            T("Enable MQTT publishing", "Luba MQTT avaldamine"));
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* MQTT server */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(g_cfg.mqtt_host, safe, sizeof(safe));
        char buf[640];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='text' name='mqtt_host' maxlength='%d' value='%s'"
            " placeholder='e.g. 192.168.1.10 or broker.local'>"
            "</label>",
            T("MQTT server hostname / IP", "MQTT serveri aadress / IP"),
            STR_LEN - 1, safe);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* MQTT port */
    {
        char buf[192];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='number' name='mqtt_port' min='1' max='65535' value='%d'>"
            "</label>",
            T("MQTT port", "MQTT port"), g_cfg.mqtt_port);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* MQTT price topic */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(g_cfg.mqtt_topic_price, safe, sizeof(safe));
        char buf[512];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='text' name='mqtt_topic_p' maxlength='%d' value='%s'>"
            "</label>",
            T("Price topic", "Hinna teema"), STR_LEN - 1, safe);
        httpd_resp_sendstr_chunk(req, buf);
    }

    /* MQTT relay topic */
    {
        char safe[STR_LEN * 6] = {0};
        html_escape(g_cfg.mqtt_topic_relay, safe, sizeof(safe));
        char buf[640];
        snprintf(buf, sizeof(buf),
            "<label>%s"
            "<input type='text' name='mqtt_topic_r' maxlength='%d' value='%s'>"
            "</label>"
            "<p class='note'>%s</p>",
            T("Relay state topic", "Relee oleku teema"), STR_LEN - 1, safe,
            T("Publishes price in c/kWh and relay state"
              " (<code>ON</code>/<code>OFF</code>) with retain=1 on every relay update.",
              "Avaldab hinna s/kWh ja relee oleku"
              " (<code>ON</code>/<code>OFF</code>) retain=1 iga relee muutuse korral."));
        httpd_resp_sendstr_chunk(req, buf);
    }

    snprintf(chunk, sizeof(chunk),
        "<button type='submit'>%s</button>"
        "</form></body></html>",
        T("Save settings", "Salvesta seaded"));
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /mqtt POST ──────────────────────────────────────────────────────────── */

static esp_err_t mqtt_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH_POST(req);
    char body[640] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[len] = '\0';

    char val[STR_LEN];

    bool mqtt_en = (strstr(body, "mqtt_en=on") || strstr(body, "mqtt_en=1"));

    char mqtt_host[STR_LEN] = {0};
    form_field(body, "mqtt_host", mqtt_host, sizeof(mqtt_host));

    form_field(body, "mqtt_port", val, sizeof(val));
    int mqtt_port = clamp(atoi(val), 1, 65535);
    if (mqtt_port == 0) mqtt_port = 1883;

    char mqtt_topic_p[STR_LEN] = {0};
    form_field(body, "mqtt_topic_p", mqtt_topic_p, sizeof(mqtt_topic_p));
    if (mqtt_topic_p[0] == '\0') strlcpy(mqtt_topic_p, "elerelay/price", sizeof(mqtt_topic_p));

    char mqtt_topic_r[STR_LEN] = {0};
    form_field(body, "mqtt_topic_r", mqtt_topic_r, sizeof(mqtt_topic_r));
    if (mqtt_topic_r[0] == '\0') strlcpy(mqtt_topic_r, "elerelay/relay", sizeof(mqtt_topic_r));

    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_cfg.mqtt_enabled = mqtt_en;
    strlcpy(g_cfg.mqtt_host,        mqtt_host,    sizeof(g_cfg.mqtt_host));
    g_cfg.mqtt_port    = mqtt_port;
    strlcpy(g_cfg.mqtt_topic_price, mqtt_topic_p, sizeof(g_cfg.mqtt_topic_price));
    strlcpy(g_cfg.mqtt_topic_relay, mqtt_topic_r, sizeof(g_cfg.mqtt_topic_relay));
    xSemaphoreGive(g_state_mutex);

    esp_err_t save_err = settings_save();
    mqtt_start();   /* restart MQTT client with new settings */

    ESP_LOGI(TAG, "MQTT settings updated: en=%d host=%s port=%d tp=%s tr=%s",
             mqtt_en, mqtt_host, mqtt_port, mqtt_topic_p, mqtt_topic_r);

    send_page_head(req, "EleRelay \xe2\x80\x94 MQTT");
    char chunk[512];
    snprintf(chunk, sizeof(chunk),
        "<h1>MQTT</h1>"
        "%s"
        "<ul style='font-size:.9rem;line-height:1.8'>"
        "<li>MQTT: <b>%s</b>%s</li>"
        "</ul>"
        "<p><a href='/mqtt'>%s</a>"
        " &nbsp; <a href='/'>%s</a></p>"
        "</body></html>",
        saved_msg(save_err),
        mqtt_en ? T("enabled","lubatud") : T("disabled","keelatud"),
        mqtt_en && mqtt_host[0] ? T(" \xe2\x80\x94 connecting...", " \xe2\x80\x94 \xc3\xbchendun...") : "",
        T("Back to MQTT settings", "Tagasi MQTT seadetesse"),
        T("Price table", "Hinnad"));
    httpd_resp_sendstr_chunk(req, chunk);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /wifi GET ───────────────────────────────────────────────────────────── */

static void send_wifi_form(httpd_req_t *req, bool is_ap)
{
    send_page_head(req, T("EleRelay \xe2\x80\x94 Network", "EleRelay \xe2\x80\x94 V&otilde;rk"));
    char safe_ssid[CRED_LEN * 6] = {0};
    html_escape(g_wifi_ssid, safe_ssid, sizeof(safe_ssid));
    char buf[1280];
    snprintf(buf, sizeof(buf),
        "<h1>&#x1F4F6; %s</h1>%s"
        "<form method='POST' action='/wifi'>"
        "<label>%s<input type='text' name='ssid' required maxlength='32'"
        " value='%s' placeholder='%s'></label>"
        "<label>%s<input type='password' name='pass' maxlength='63'"
        " placeholder='%s'></label>"
        "<h2>%s</h2>"
        "<div class='chk'><input type='radio' name='ip_mode' value='dhcp' id='ipd'%s"
        " onchange='elIpToggle()'><label for='ipd' style='margin:0'>%s</label></div>"
        "<div class='chk'><input type='radio' name='ip_mode' value='static' id='ips'%s"
        " onchange='elIpToggle()'><label for='ips' style='margin:0'>%s</label></div>",
        is_ap ? T("First Setup", "Esmane seadistus") : T("Network Settings", "V&otilde;rgu seaded"),
        is_ap ? T("<p>Connect this device to your WiFi network.</p>",
                  "<p>&#xDC;hendage seade WiFi v&otilde;rguga.</p>") : "",
        T("Network (SSID)", "V&otilde;rk (SSID)"),
        safe_ssid,
        T("Network name", "V&otilde;rgu nimi"),
        T("Password", "Parool"),
        T("Leave blank to keep the current password (same SSID) or for an open network",
          "J&auml;ta t&uuml;hjaks, et s&auml;ilitada praegune parool (sama SSID) v&otilde;i avatud v&otilde;rgu puhul"),
        T("IP Configuration", "IP seadistus"),
        g_cfg.ip_mode == 0 ? " checked" : "",
        T("DHCP (automatic)", "DHCP (automaatne)"),
        g_cfg.ip_mode == 1 ? " checked" : "",
        T("Static IP", "Staatiline IP"));
    httpd_resp_sendstr_chunk(req, buf);

    httpd_resp_sendstr_chunk(req, "<div id='ipfields'>");
    const struct { const char *label, *name, *value, *placeholder; } ipf[] = {
        { T("IP Address", "IP aadress"),               "ip_addr", g_cfg.static_ip,   "192.168.1.50"  },
        { T("Subnet Mask", "Alamv&otilde;rgu mask"),   "ip_mask", g_cfg.static_mask, "255.255.255.0" },
        { T("Gateway", "L&uuml;&uuml;s"),              "ip_gw",   g_cfg.static_gw,   "192.168.1.1"   },
        { T("DNS", "DNS"),                             "ip_dns",  g_cfg.static_dns,  "192.168.1.1"   },
    };
    for (size_t k = 0; k < sizeof(ipf) / sizeof(ipf[0]); k++) {
        char safe[IP_STR_LEN * 6];
        html_escape(ipf[k].value, safe, sizeof(safe));
        snprintf(buf, sizeof(buf),
            "<label>%s<input type='text' name='%s' maxlength='15' value='%s'"
            " placeholder='%s'></label>",
            ipf[k].label, ipf[k].name, safe, ipf[k].placeholder);
        httpd_resp_sendstr_chunk(req, buf);
    }
    snprintf(buf, sizeof(buf),
        "</div>"
        "<script>function elIpToggle(){"
        "document.getElementById('ipfields').style.display="
        "document.getElementById('ips').checked?'block':'none';}"
        "elIpToggle();</script>"
        "<button type='submit'>%s</button>"
        "</form>"
        "<p class='note'>%s</p>",
        T("Save &amp; Restart", "Salvesta &amp; Taask&auml;ivita"),
        T("Device restarts and tries to connect. Returns to AP setup mode if it fails.",
          "Seade taask&auml;ivitub ja proovib &uuml;henduda. Kui ei &otilde;nnestu, l&auml;heb AP-re&zcaron;iimi."));
    httpd_resp_sendstr_chunk(req, buf);
    if (!is_ap) {
        char back[128];
        snprintf(back, sizeof(back), "<p><a href='/'>&#x2190; %s</a></p>",
                 T("Back to price table", "Tagasi hindade lehele"));
        httpd_resp_sendstr_chunk(req, back);
    }
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    send_wifi_form(req, g_ap_mode);
    return ESP_OK;
}

/* ── /wifi POST ──────────────────────────────────────────────────────────── */

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH_POST(req);
    char body[768] = {0};
    int  len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[len] = '\0';

    char ssid[CRED_LEN] = {0}, pass[CRED_LEN] = {0};
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_FAIL;
    }

    /* The form never pre-fills the password. A blank password for the
     * currently stored SSID means "keep it", so changing only the IP
     * settings does not wipe the password. */
    if (pass[0] == '\0') {
        char old_ssid[CRED_LEN] = {0}, old_pass[CRED_LEN] = {0};
        if (creds_load(old_ssid, old_pass) == ESP_OK && strcmp(old_ssid, ssid) == 0)
            strlcpy(pass, old_pass, sizeof(pass));
    }

    char ip_mode[8] = {0};
    char ip_addr[IP_STR_LEN] = {0}, ip_mask[IP_STR_LEN] = {0};
    char ip_gw[IP_STR_LEN]   = {0}, ip_dns[IP_STR_LEN]  = {0};
    form_field(body, "ip_mode", ip_mode, sizeof(ip_mode));
    form_field(body, "ip_addr", ip_addr, sizeof(ip_addr));
    form_field(body, "ip_mask", ip_mask, sizeof(ip_mask));
    form_field(body, "ip_gw",   ip_gw,   sizeof(ip_gw));
    form_field(body, "ip_dns",  ip_dns,  sizeof(ip_dns));

    bool   want_static = (strcmp(ip_mode, "static") == 0);
    ip4_addr_t addr;
    if (want_static && (!ip4addr_aton(ip_addr, &addr) || !ip4addr_aton(ip_mask, &addr))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid static IP configuration");
        return ESP_FAIL;
    }
    if (ip_addr[0] && !ip4addr_aton(ip_addr, &addr)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid IP address");
        return ESP_FAIL;
    }
    if (ip_mask[0] && !ip4addr_aton(ip_mask, &addr)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid subnet mask");
        return ESP_FAIL;
    }
    if (ip_gw[0]  && !ip4addr_aton(ip_gw, &addr)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid gateway");
        return ESP_FAIL;
    }
    if (ip_dns[0] && !ip4addr_aton(ip_dns, &addr)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid DNS");
        return ESP_FAIL;
    }

    g_cfg.ip_mode = want_static ? 1 : 0;
    strlcpy(g_cfg.static_ip,   ip_addr, sizeof(g_cfg.static_ip));
    strlcpy(g_cfg.static_mask, ip_mask, sizeof(g_cfg.static_mask));
    strlcpy(g_cfg.static_gw,   ip_gw,   sizeof(g_cfg.static_gw));
    strlcpy(g_cfg.static_dns,  ip_dns,  sizeof(g_cfg.static_dns));
    /* Restart only if both the IP settings and the credentials were stored,
     * otherwise the device would come back up with a half-applied config. */
    esp_err_t err = settings_save();
    if (err == ESP_OK) err = creds_save(ssid, pass);
    send_page_head(req, T("EleRelay \xe2\x80\x94 Network", "EleRelay \xe2\x80\x94 V&otilde;rk"));
    if (err == ESP_OK) {
        char safe[CRED_LEN * 6] = {0};
        html_escape(ssid, safe, sizeof(safe));
        char html[640];
        snprintf(html, sizeof(html),
            "<h1>&#x2714; %s</h1>"
            "<p>%s <b>%s</b>... %s</p>"
            "<p class='note'>%s</p>"
            "</body></html>",
            T("Saved!", "Salvestatud!"),
            T("Connecting to", "&Uuml;hendan v&otilde;rguga"), safe,
            T("Restarting.", "Taask&auml;ivitan."),
            T("Returns to AP setup mode if connection fails.",
              "L&auml;heb AP-re&zcaron;iimi, kui &uuml;hendus nurjub."));
        httpd_resp_sendstr_chunk(req, html);
        httpd_resp_sendstr_chunk(req, NULL);
        app_restart_later();
    } else {
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg),
            "<h1>&#x274C; %s</h1><p>%s</p></body></html>",
            T("Error", "Viga"),
            T("Failed to save network settings. Nothing was changed.",
              "V&otilde;rgu seadete salvestamine eba&otilde;nnestus. Midagi ei muudetud."));
        httpd_resp_sendstr_chunk(req, errmsg);
        httpd_resp_sendstr_chunk(req, NULL);
    }
    return ESP_OK;
}

/* ── / (price table) ─────────────────────────────────────────────────────── */

static esp_err_t web_get_handler(httpd_req_t *req)
{
    char chunk[1024];

    send_page_head(req, "EleRelay");
    {
        char h1[64];
        snprintf(h1, sizeof(h1), "<h1>&#x26A1; %s</h1>", "EleRelay");
        httpd_resp_sendstr_chunk(req, h1);
    }

    /* Snapshot + forecast. Static: ~2 KB, and the HTTP server runs one
     * handler at a time. */
    static app_forecast_t fc;
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    app_forecast(&fc, now);
    bool   relay_on   = fc.relay_on;
    int    count      = fc.count;
    time_t last_fetch = fc.last_fetch;
    bool   inv        = g_cfg.relay_inv;
    int    win_hours  = g_cfg.hours_window;
    const hour_slot_t *local = fc.slots;

    char time_str[32], fetch_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S %Z", &ti);

    if (last_fetch) {
        struct tm tf; localtime_r(&last_fetch, &tf);
        strftime(fetch_str, sizeof(fetch_str), "%H:%M:%S", &tf);
    } else {
        strlcpy(fetch_str, "never", sizeof(fetch_str));
    }

    char safe_ssid[CRED_LEN * 6] = {0};
    html_escape(g_wifi_ssid, safe_ssid, sizeof(safe_ssid));

    snprintf(chunk, sizeof(chunk),
        "<p>%s <b>%s</b></p>"
        "<p>%s <span class='status %s'>%s</span>%s</p>"
        "<p class='meta'>IP: %s &nbsp;|&nbsp; WiFi: %s"
        " &nbsp;|&nbsp; %s %s"
        " &nbsp;|&nbsp; %s %dh, %s %dh, %s %02d:00</p>",
        T("Time:", "Aeg:"), time_str,
        T("Relay:", "Relee:"), relay_on ? "on" : "off", relay_on ? "ON" : "OFF",
        inv ? T(" <span style='font-size:.8rem;color:#888'>(inverted)</span>",
                " <span style='font-size:.8rem;color:#888'>(p&ouml;&ouml;ratud)</span>") : "",
        g_ip, safe_ssid,
        T("Last fetch:", "Viimane p&auml;ring:"), fetch_str,
        T("Window:", "Aken:"), g_cfg.hours_window,
        T("cheap:", "odav:"), g_cfg.cheap_hours,
        T("fetch:", "p&auml;ring:"), g_cfg.fetch_hour);
    httpd_resp_sendstr_chunk(req, chunk);
    {
        char fbtn[256];
        snprintf(fbtn, sizeof(fbtn),
            "<form method='POST' action='/fetch' style='margin:10px 0'>"
            "<button type='submit' style='width:auto;padding:7px 18px;"
            "font-size:.9rem'>&#x21BB; %s</button></form>",
            T("Update prices now", "Uuenda hinnad"));
        httpd_resp_sendstr_chunk(req, fbtn);
    }
    if (app_fetch_busy()) {
        /* Reload until the background fetch has finished */
        char note[192];
        snprintf(note, sizeof(note),
            "<p class='note'>&#x21BB; %s</p>"
            "<script>setTimeout(function(){location.reload();},3000);</script>",
            T("Updating prices&hellip;", "Uuendan hindu&hellip;"));
        httpd_resp_sendstr_chunk(req, note);
    }

    if (count == 0) {
        char nomsg[128];
        snprintf(nomsg, sizeof(nomsg), "<p><i>%s</i></p>",
            T("No price data yet &mdash; fetching&hellip;",
              "Hinna andmed puuduvad &mdash; laadin&hellip;"));
        httpd_resp_sendstr_chunk(req, nomsg);
    } else {
        /* Skip 15-min slots that have already ended */
        int start = fc.first;
        int show  = count - start;

        snprintf(chunk, sizeof(chunk),
            "<p class='meta'>%s %d &times; 15min.</p>"
            "<table><tr><th>%s</th><th>%s</th><th>%s</th></tr>",
            T("Showing", "N&auml;itan"), show,
            T("Time (local)", "Aeg (kohalik)"),
            T("Price (c/kWh)", "Hind (s/kWh)"),
            T("Relay", "Relee"));
        httpd_resp_sendstr_chunk(req, chunk);

        time_t cur_slot = (now / SLOT_SECS) * SLOT_SECS;
        time_t win_base = local[0].ts;
        int    win_secs = (win_hours > 0 ? win_hours : 1) * 3600;
        for (int i = start; i < start + show; i++) {
            /* Insert window separator row at each hours-window boundary */
            int this_win = (int)((local[i].ts - win_base) / win_secs);
            int prev_win = (i > start) ? (int)((local[i-1].ts - win_base) / win_secs) : this_win - 1;
            if (this_win != prev_win) {
                time_t ws_ts = win_base + (time_t)this_win * win_secs;
                struct tm tws; localtime_r(&ws_ts, &tws);
                char ws_str[20]; strftime(ws_str, sizeof(ws_str), "%Y-%m-%d %H:%M", &tws);
                snprintf(chunk, sizeof(chunk),
                    "<tr class='win-sep'><td colspan='3'>"
                    "&#x1F5D3; %s %d &mdash; %s</td></tr>",
                    T("Window", "Aken"), this_win + 1, ws_str);
                httpd_resp_sendstr_chunk(req, chunk);
            }

            struct tm th; localtime_r(&local[i].ts, &th);
            char hr[20]; strftime(hr, sizeof(hr), "%Y-%m-%d %H:%M", &th);
            bool is_cur = (local[i].ts == cur_slot);
            bool ron    = fc.on[i];   /* forecast relay state (min-run aware) */
            float cprice  = local[i].price / 10.0f;
            int this_h = (int)(local[i].ts / 3600);
            int prev_h = (i > start)            ? (int)(local[i-1].ts / 3600) : -1;
            int next_h = (i < start + show - 1) ? (int)(local[i+1].ts / 3600) : -1;
            char row_cls[64];
            snprintf(row_cls, sizeof(row_cls), "hg %s%s%s%s",
                ron ? "cheap" : "exp",
                is_cur ? " cur" : "",
                prev_h != this_h ? " hs" : "",
                next_h != this_h ? " he" : "");
            snprintf(chunk, sizeof(chunk),
                "<tr class='%s'>"
                "<td>%s</td><td>%.3f</td>"
                "<td class='%s'>%s</td></tr>",
                row_cls,
                hr, cprice,
                ron ? "ron" : "roff",
                ron ? "ON" : "OFF");
            httpd_resp_sendstr_chunk(req, chunk);
        }
        httpd_resp_sendstr_chunk(req, "</table>");
    }
    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /ota GET ────────────────────────────────────────────────────────────── */

static esp_err_t ota_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    send_page_head(req, "EleRelay \xe2\x80\x94 OTA Update");
    httpd_resp_sendstr_chunk(req, "<h1>&#x1F4E6; OTA Firmware Update</h1>");

    /* Current partition info */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *update  = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t  *desc    = esp_app_get_description();

    char info[512];
    snprintf(info, sizeof(info),
        "<p class='meta'>Running: <b>%s</b> &nbsp;|&nbsp; "
        "Version: <b>%s</b> &nbsp;|&nbsp; "
        "Built: <b>%s %s</b></p>"
        "<p class='meta'>Next update slot: <b>%s</b></p>",
        running ? running->label : "?",
        desc->version,
        desc->date, desc->time,
        update ? update->label : "none");
    httpd_resp_sendstr_chunk(req, info);

    httpd_resp_sendstr_chunk(req,
        "<p>Select a <code>.bin</code> firmware image built with"
        " <code>idf.py build</code> (<code>build/elering_relay.bin</code>).</p>"
        "<input type='file' id='fw' accept='.bin'>"
        "<p id='fsize' class='note'></p>"
        "<button id='btn' onclick='doOta()'>Upload &amp; Flash</button>"
        "<progress id='bar' value='0' max='100' style='display:none'></progress>"
        "<p id='status' class='ota-status'></p>"
        "<script>"
        "document.getElementById('fw').onchange=function(){"
          "var f=this.files[0];"
          "if(f)document.getElementById('fsize').textContent="
            "(f.size/1024).toFixed(1)+' KB';"
        "};"
        "function doOta(){"
          "var f=document.getElementById('fw').files[0];"
          "if(!f){alert('Select a .bin file first');return;}"
          "var btn=document.getElementById('btn'),"
              "bar=document.getElementById('bar'),"
              "st=document.getElementById('status');"
          "btn.disabled=true;"
          "bar.style.display='block';"
          "st.textContent='Starting upload...';"
          "var xhr=new XMLHttpRequest();"
          "xhr.open('POST','/ota');"
          "xhr.setRequestHeader('Content-Type','application/octet-stream');"
          "xhr.upload.onprogress=function(e){"
            "if(e.lengthComputable){"
              "var pct=Math.round(e.loaded/e.total*100);"
              "bar.value=pct;"
              "st.textContent='Uploading... '+pct+'%';"
            "}"
          "};"
          "xhr.onload=function(){"
            "bar.value=100;"
            "if(xhr.status===200){"
              "st.style.color='#1b5e20';"
              "st.textContent='\u2714 Done! Rebooting \u2014 reconnect in ~5 s';"
              "setTimeout(function(){window.location.href='/';},7000);"
            "}else{"
              "st.style.color='#b71c1c';"
              "st.textContent='\u2718 Failed: '+xhr.responseText;"
              "btn.disabled=false;"
            "}"
          "};"
          "xhr.onerror=function(){"
            "st.style.color='#b71c1c';"
            "st.textContent='\u2718 Upload error';"
            "btn.disabled=false;"
          "};"
          "xhr.send(f);"
        "}"
        "</script>"
        "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

/* ── /ota POST ───────────────────────────────────────────────────────────── */

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH_POST(req);
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition found");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(2048);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int written   = 0;
    bool failed   = false;

    while (remaining > 0) {
        int to_recv = remaining < 2048 ? remaining : 2048;
        int recv = httpd_req_recv(req, buf, to_recv);
        if (recv <= 0) {
            ESP_LOGE(TAG, "OTA recv error: %d (remaining %d)", recv, remaining);
            failed = true;
            break;
        }
        err = esp_ota_write(ota_handle, buf, recv);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            failed = true;
            break;
        }
        remaining -= recv;
        written   += recv;
    }
    free(buf);

    if (failed) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA success — %d bytes written to %s — rebooting", written, update->label);
    httpd_resp_sendstr(req, "OK");
    app_restart_later();
    return ESP_OK;
}

/* ── /fetch POST ─────────────────────────────────────────────────────────── */

static esp_err_t fetch_post_handler(httpd_req_t *req)
{
    /* Public price-table button: no login, but no cross-site triggering */
    if (!origin_ok(req)) return ESP_FAIL;
    /* The network task fetches in the background (up to ~20 s); the price
     * table shows a note and reloads itself until it is done. */
    app_request_fetch();
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

/* ── Web server ──────────────────────────────────────────────────────────── */

httpd_handle_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.stack_size       = 8192;
    cfg.max_uri_handlers = 14;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server"); return NULL;
    }

    static const httpd_uri_t u_root_sta  = { "/",         HTTP_GET,  web_get_handler,      NULL };
    static const httpd_uri_t u_root_ap   = { "/",         HTTP_GET,  wifi_get_handler,     NULL };
    static const httpd_uri_t u_wifi_get  = { "/wifi",     HTTP_GET,  wifi_get_handler,     NULL };
    static const httpd_uri_t u_wifi_post = { "/wifi",     HTTP_POST, wifi_post_handler,    NULL };
    static const httpd_uri_t u_rel_get   = { "/relay",    HTTP_GET,  relay_get_handler,    NULL };
    static const httpd_uri_t u_rel_post  = { "/relay",    HTTP_POST, relay_post_handler,   NULL };
    static const httpd_uri_t u_set_get   = { "/settings", HTTP_GET,  settings_get_handler, NULL };
    static const httpd_uri_t u_set_post  = { "/settings", HTTP_POST, settings_post_handler,NULL };
    static const httpd_uri_t u_mqtt_get  = { "/mqtt",     HTTP_GET,  mqtt_get_handler,     NULL };
    static const httpd_uri_t u_mqtt_post = { "/mqtt",     HTTP_POST, mqtt_post_handler,    NULL };
    static const httpd_uri_t u_ota_get    = { "/ota",    HTTP_GET,  ota_get_handler,    NULL };
    static const httpd_uri_t u_ota_post   = { "/ota",    HTTP_POST, ota_post_handler,   NULL };
    static const httpd_uri_t u_fetch_post = { "/fetch",  HTTP_POST, fetch_post_handler, NULL };

    if (g_ap_mode) {
        httpd_register_uri_handler(srv, &u_root_ap);
    } else {
        httpd_register_uri_handler(srv, &u_root_sta);
        httpd_register_uri_handler(srv, &u_wifi_get);
        httpd_register_uri_handler(srv, &u_rel_get);
        httpd_register_uri_handler(srv, &u_rel_post);
        httpd_register_uri_handler(srv, &u_set_get);
        httpd_register_uri_handler(srv, &u_set_post);
        httpd_register_uri_handler(srv, &u_mqtt_get);
        httpd_register_uri_handler(srv, &u_mqtt_post);
        httpd_register_uri_handler(srv, &u_ota_get);
        httpd_register_uri_handler(srv, &u_ota_post);
        httpd_register_uri_handler(srv, &u_fetch_post);
    }
    httpd_register_uri_handler(srv, &u_wifi_post);

    ESP_LOGI(TAG, "HTTP server ready at http://%s/", g_ip);
    return srv;
}
