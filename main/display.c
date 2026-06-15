/*
 * display.c — ILI9341 display driver for ESP-2432S028 (Cheap Yellow Display)
 *
 * Landscape 320×240, RGB565.
 * Layout:
 *   y=  0..23  header: date + HH:MM:SS (both scale×2)
 *   y= 24..25  separator
 *   y= 26..99  relay status (big) + current price
 *   y=100..101 separator
 *   y=102..121 WiFi / AP info
 *   y=122..123 separator
 *   y=124..239 price bar chart (next DISP_MAX_BARS slots)
 */

#include "display.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "touch";

/* ── ESP-2432S028 pin map ───────────────────────────────────────────────── */
#define LCD_MOSI   13
#define LCD_SCLK   14
#define LCD_CS     15
#define LCD_DC      2
#define LCD_RST    -1   /* Not connected; rely on software reset */
#define LCD_BL     21

#define LEDC_BASE_FREQ      5000
#define LCD_BACK_LIGHT_PIN  21
#define LEDC_BL_CHANNEL     LEDC_CHANNEL_0
#define LEDC_BL_TIMER       LEDC_TIMER_0
#define LEDC_BL_RESOLUTION  LEDC_TIMER_8_BIT

#define LCD_W     320
#define LCD_H     240
#define LCD_HOST  SPI2_HOST

/* ── Display command codes (shared subset) ──────────────────────────────── */
#define ILI_SWRESET  0x01
#define ILI_SLPOUT   0x11
#define ILI_INVOFF   0x20
#define ILI_INVON    0x21
#define ILI_DISPON   0x29
#define ILI_CASET    0x2A
#define ILI_RASET    0x2B
#define ILI_RAMWR    0x2C
#define ILI_MADCTL   0x36
#define ILI_COLMOD   0x3A
/* ILI9341-only */
#define ILI_FRMCTR1  0xB1
#define ILI_DFUNCTR  0xB6
#define ILI_PWCTR1   0xC0
#define ILI_PWCTR2   0xC1
#define ILI_VMCTR1   0xC5
#define ILI_VMCTR2   0xC7
#define ILI_GMCTRP1  0xE0
#define ILI_GMCTRN1  0xE1

/*
 * ILI9341 MADCTL for landscape 320×240:
 *   MV=1 (bit5) — swap row/col → landscape
 *   BGR=1 (bit3) — ILI9341 native color order
 * Alternatives: 0x68 (X-mirror), 0xA8 (Y-mirror), 0xE8 (180°)
 *
 * ST7789 MADCTL for landscape 320×240:
 *   MX=1 (bit6), MV=1 (bit5) — X-mirror + swap → landscape
 *   No BGR bit — ST7789 is RGB-native
 * Alternatives: 0x20 (no mirror), 0xA0 (Y-mirror), 0xC0 (180°)
 */
#define ILI9341_MADCTL  0x60
#define ST7789_MADCTL   0x20

#ifdef CONFIG_LCD_ST7789
#define LCD_MADCTL  ST7789_MADCTL
#else
#define LCD_MADCTL  ILI9341_MADCTL
#endif

/* ── RGB565 colour palette ──────────────────────────────────────────────── */
/* R and B channels are pre-swapped to match this panel's physical subpixel order */
#define C_BLACK   0x0000u
#define C_WHITE   0xFFFFu
#define C_GREEN   0x07E0u
#define C_DKGREEN 0x05E0u   /* G=47/63, ~75% */
#define C_RED     0x001Fu   /* R↔B: was 0xF800 */
#define C_DKRED   0x0017u   /* R↔B, ~75%: was 0x000F */
#define C_BLUE    0xF800u   /* R↔B: was 0x001F */
#define C_NAVY    0x7800u   /* R↔B: was 0x000F */
#define C_YELLOW  0x07FFu   /* R↔B: was 0xFFE0 */
#define C_ORANGE  0x053Fu   /* R↔B: was 0xFD20 */
#define C_GRAY    0x7BEFu   /* R=B, unchanged */
#define C_DKGRAY  0x39E7u   /* R=B, unchanged */
#define C_CYAN    0xFFE0u   /* R↔B: was 0x07FF */

/* ── 8×8 bitmap font (ASCII 32–126) ─────────────────────────────────────── */
/* Each character is 8 bytes, one per row, MSB = leftmost pixel. */
static const uint8_t font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 32 ' ' */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 33 '!' */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* 34 '"' */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* 35 '#' */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* 36 '$' */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* 37 '%' */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* 38 '&' */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* 39 ''' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* 40 '(' */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* 41 ')' */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 42 '*' */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* 43 '+' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* 44 ',' */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* 45 '-' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* 46 '.' */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* 47 '/' */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 48 '0' */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 49 '1' */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 50 '2' */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 51 '3' */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 52 '4' */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 53 '5' */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 54 '6' */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 55 '7' */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 56 '8' */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 57 '9' */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* 58 ':' */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* 59 ';' */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* 60 '<' */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* 61 '=' */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 62 '>' */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* 63 '?' */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* 64 '@' */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* 65 'A' */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* 66 'B' */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* 67 'C' */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* 68 'D' */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* 69 'E' */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* 70 'F' */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* 71 'G' */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* 72 'H' */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 73 'I' */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* 74 'J' */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* 75 'K' */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* 76 'L' */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* 77 'M' */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* 78 'N' */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* 79 'O' */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* 80 'P' */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* 81 'Q' */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* 82 'R' */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* 83 'S' */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 84 'T' */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* 85 'U' */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 86 'V' */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 87 'W' */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* 88 'X' */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* 89 'Y' */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* 90 'Z' */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* 91 '[' */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* 92 '\' */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* 93 ']' */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* 94 '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 95 '_' */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* 96 '`' */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* 97 'a' */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* 98 'b' */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* 99 'c' */
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, /* 100 'd' */
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, /* 101 'e' */
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00}, /* 102 'f' */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* 103 'g' */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* 104 'h' */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* 105 'i' */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* 106 'j' */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* 107 'k' */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 108 'l' */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* 109 'm' */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* 110 'n' */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* 111 'o' */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* 112 'p' */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* 113 'q' */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* 114 'r' */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* 115 's' */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* 116 't' */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* 117 'u' */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 118 'v' */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* 119 'w' */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* 120 'x' */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* 121 'y' */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* 122 'z' */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* 123 '{' */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 124 '|' */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* 125 '}' */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* 126 '~' */
};

/* ── 16×16 font: Scale2x upscale of font8x8, built at init ──────────────── */
/* 16 rows × 2 bytes per row; bit 15 of uint16 = leftmost pixel */
static uint8_t font16x16[95][32];

static void build_font16x16(void)
{
    for (int g = 0; g < 95; g++) {
        const uint8_t *src = font8x8[g];
        uint8_t *dst = font16x16[g];
        memset(dst, 0, 32);
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                int E = (src[row]              >> col)       & 1;
                int B = (row > 0) ? (src[row-1] >> col)       & 1 : 0;
                int H = (row < 7) ? (src[row+1] >> col)       & 1 : 0;
                int D = (col > 0) ? (src[row]   >> (col - 1)) & 1 : 0;
                int F = (col < 7) ? (src[row]   >> (col + 1)) & 1 : 0;

                int tl, tr, bl, br;
                if (B != H && D != F) {
                    tl = (D == B) ? B : E;
                    tr = (B == F) ? F : E;
                    bl = (D == H) ? D : E;
                    br = (H == F) ? H : E;
                } else {
                    tl = tr = bl = br = E;
                }

                int out[2][2] = {{tl, tr}, {bl, br}};
                for (int dr = 0; dr < 2; dr++) {
                    for (int dc = 0; dc < 2; dc++) {
                        if (out[dr][dc]) {
                            int drow = row * 2 + dr;
                            int dcol = col * 2 + dc;
                            dst[drow * 2 + dcol / 8] |= (uint8_t)(1 << (7 - dcol % 8));
                        }
                    }
                }
            }
        }
    }
}

/* ── Globals ────────────────────────────────────────────────────────────── */
static esp_lcd_panel_io_handle_t s_io = NULL;
static int s_disp_lang = LANG_EN;
/* Serializes LCD SPI access between display_update() (controller_task)
 * and display_blink_offline() (offline_blink_task). */
static SemaphoreHandle_t s_lcd_mutex = NULL;

void display_set_lang(int lang) { s_disp_lang = lang; }

/* Picks between English and Estonian string (LCD must stay ASCII) */
#define DT(en, et) (s_disp_lang == LANG_ET ? (et) : (en))

/* ── XPT2046 touch controller ────────────────────────────────────────────── */
#define TOUCH_CLK    25
#define TOUCH_CS     33
#define TOUCH_MOSI   32
#define TOUCH_MISO   39
#define TOUCH_IRQ    36
#define TOUCH_HOST   SPI3_HOST

/* Raw ADC at screen edges (calibrated from corner taps) */
#define T_X_MIN   240   /* 0x90 at left edge  */
#define T_X_MAX  3625   /* 0x90 at right edge */
#define T_Y_MIN   397   /* 0xD0 at top edge   */
#define T_Y_MAX  3717   /* 0xD0 at bottom edge*/

static spi_device_handle_t s_touch = NULL;

static uint16_t xpt_sample(uint8_t cmd)
{
    uint8_t tx[3] = { cmd, 0, 0 };
    uint8_t rx[3] = { 0, 0, 0 };
    spi_transaction_t t = { .length = 24, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(s_touch, &t);
    return ((uint16_t)(rx[1] << 8) | rx[2]) >> 3;
}

void display_touch_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num   = TOUCH_MOSI,
        .miso_io_num   = TOUCH_MISO,
        .sclk_io_num   = TOUCH_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    spi_bus_initialize(TOUCH_HOST, &bus, SPI_DMA_DISABLED);

    spi_device_interface_config_t dev = {
        .clock_speed_hz = 2 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = TOUCH_CS,
        .queue_size     = 1,
    };
    spi_bus_add_device(TOUCH_HOST, &dev, &s_touch);
    gpio_set_direction(TOUCH_IRQ, GPIO_MODE_INPUT);
}

bool display_touch_read(int *sx, int *sy)
{
    if (!s_touch) return false;
    if (gpio_get_level(TOUCH_IRQ)) return false;  /* not touched */

    /* Average 4 samples per axis.
     * On CYD (MADCTL=0x28): 0x90 → screen X, 0xD0 → screen Y (inverted). */
    int r90 = 0, rd0 = 0;
    for (int i = 0; i < 4; i++) {
        r90 += xpt_sample(0x90);
        rd0 += xpt_sample(0xD0);
    }
    r90 /= 4; rd0 /= 4;

    /* Log raw values so calibration constants can be tuned */
    ESP_LOGI(TAG, "raw 0x90=%d 0xD0=%d", r90, rd0);

    /* Map to screen coordinates:
     *   X: 0x90 low→left, high→right
     *   Y: 0xD0 high→top, low→bottom (inverted) */
    int x = (r90 - T_X_MIN) * (LCD_W - 1) / (T_X_MAX - T_X_MIN);
    int y = (rd0 - T_Y_MIN) * (LCD_H - 1) / (T_Y_MAX - T_Y_MIN);
    if (x < 0) x = 0;
    if (x >= LCD_W) x = LCD_W - 1;
    if (y < 0) y = 0;
    if (y >= LCD_H) y = LCD_H - 1;
    *sx = x; *sy = y;
    return true;
}

/*
 * Stripe framebuffer: 30 rows × 320 cols × 2 bytes = 19,200 bytes.
 * The full 320×240 framebuffer (153 KB) does not fit in ESP32 DRAM.
 * We render the scene 8 times, once per stripe, clipping each draw call
 * to the current stripe's Y range.
 */
#define STRIPE_H 30
static uint16_t s_fb[LCD_W * STRIPE_H];
static int s_sy0 = 0;  /* top Y of the stripe currently being filled */

/* ── Helper: swap bytes for RGB565 (SPI sends MSB first) ─────────────────── */
static inline uint16_t swap16(uint16_t c)
{
    return (c >> 8) | (c << 8);
}

/* ── Send ILI9341 command with 0 or more parameter bytes ─────────────────── */
static void ili_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    esp_lcd_panel_io_tx_param(s_io, cmd, data, len);
}

/* ── Draw helpers (all clip to current stripe s_sy0 .. s_sy0+STRIPE_H-1) ── */
static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    uint16_t sc = swap16(color);
    for (int row = y; row < y + h && row < LCD_H; row++) {
        int srow = row - s_sy0;
        if (srow < 0 || srow >= STRIPE_H) continue;
        for (int col = x; col < x + w && col < LCD_W; col++)
            s_fb[srow * LCD_W + col] = sc;
    }
}

/* Draw single char at pixel position, scale ×1 or larger */
static void draw_char(int x, int y, char ch, uint16_t fg, uint16_t bg, int scale)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *glyph = font8x8[ch - 32];
    uint16_t sfg = swap16(fg), sbg = swap16(bg);
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            uint16_t c = (glyph[row] & (0x01 << col)) ? sfg : sbg;
            for (int sy = 0; sy < scale; sy++) {
                int py = y + row * scale + sy;
                int srow = py - s_sy0;
                if (srow < 0 || srow >= STRIPE_H) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    if (px >= 0 && px < LCD_W)
                        s_fb[srow * LCD_W + px] = c;
                }
            }
        }
    }
}

static void draw_str(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    while (*s) {
        draw_char(x, y, *s++, fg, bg, scale);
        x += 8 * scale;
        if (x + 8 * scale > LCD_W) break;
    }
}

static void draw_char_16(int x, int y, char ch, uint16_t fg, uint16_t bg, int scale)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *glyph = font16x16[ch - 32];
    uint16_t sfg = swap16(fg), sbg = swap16(bg);
    for (int row = 0; row < 16; row++) {
        uint16_t bits = ((uint16_t)glyph[row * 2] << 8) | glyph[row * 2 + 1];
        for (int col = 0; col < 16; col++) {
            uint16_t c = (bits & (0x8000u >> col)) ? sfg : sbg;
            for (int sy = 0; sy < scale; sy++) {
                int py = y + row * scale + sy;
                int srow = py - s_sy0;
                if (srow < 0 || srow >= STRIPE_H) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    if (px >= 0 && px < LCD_W)
                        s_fb[srow * LCD_W + px] = c;
                }
            }
        }
    }
}

static void draw_str_16(int x, int y, const char *s, uint16_t fg, uint16_t bg, int scale)
{
    while (*s) {
        draw_char_16(x, y, *s++, fg, bg, scale);
        x += 16 * scale;
        if (x + 16 * scale > LCD_W) break;
    }
}

/* ── Flush current stripe (s_sy0 .. s_sy0+rows-1) to display ───────────── */
static void flush_stripe(int rows)
{
    /* Column address: 0 .. LCD_W-1 */
    uint8_t caset[4] = {0, 0, (LCD_W - 1) >> 8, (LCD_W - 1) & 0xFF};
    ili_cmd(ILI_CASET, caset, 4);

    /* Row address: s_sy0 .. s_sy0+rows-1 */
    int y1 = s_sy0 + rows - 1;
    uint8_t raset[4] = {(uint8_t)(s_sy0 >> 8), (uint8_t)(s_sy0 & 0xFF),
                        (uint8_t)(y1 >> 8),    (uint8_t)(y1 & 0xFF)};
    ili_cmd(ILI_RASET, raset, 4);

    /* Use tx_param (synchronous/polling) so the buffer is not overwritten
     * while DMA is still reading it. tx_color is async and would corrupt
     * the next stripe's memset.                                            */
    esp_lcd_panel_io_tx_param(s_io, ILI_RAMWR, s_fb, LCD_W * rows * 2);
}

/* ── Offline "!" icon: drawn directly (bypasses stripe fb) for fast blink ── */
#define ICON_X 126
#define ICON_Y 26
#define ICON_W 74
#define ICON_H 73
static uint16_t s_icon_fb[ICON_W * ICON_H];

void display_blink_offline(bool visible)
{
    uint16_t bg = swap16(C_BLACK);
    for (int i = 0; i < ICON_W * ICON_H; i++) s_icon_fb[i] = bg;

    if (visible) {
        uint16_t fg = swap16(C_RED);
        /* "!" as bar (top) + square (bottom), centered */
        const int bar_w = 16, bar_h = 40, gap = 8, sq = 16;
        const int ox = (ICON_W - bar_w) / 2;
        const int oy = (ICON_H - (bar_h + gap + sq)) / 2;
        for (int y = oy; y < oy + bar_h; y++)
            for (int x = ox; x < ox + bar_w; x++)
                s_icon_fb[y * ICON_W + x] = fg;
        for (int y = oy + bar_h + gap; y < oy + bar_h + gap + sq; y++)
            for (int x = ox; x < ox + sq; x++)
                s_icon_fb[y * ICON_W + x] = fg;
    }

    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    uint8_t caset[4] = {(ICON_X >> 8) & 0xFF, ICON_X & 0xFF,
                         ((ICON_X + ICON_W - 1) >> 8) & 0xFF, (ICON_X + ICON_W - 1) & 0xFF};
    ili_cmd(ILI_CASET, caset, 4);
    uint8_t raset[4] = {(ICON_Y >> 8) & 0xFF, ICON_Y & 0xFF,
                         ((ICON_Y + ICON_H - 1) >> 8) & 0xFF, (ICON_Y + ICON_H - 1) & 0xFF};
    ili_cmd(ILI_RASET, raset, 4);
    esp_lcd_panel_io_tx_param(s_io, ILI_RAMWR, s_icon_fb, ICON_W * ICON_H * 2);
    xSemaphoreGive(s_lcd_mutex);
}

/* ── ILI9341 initialisation sequence ───────────────────────────────────── */
static void ili9341_init(void)
{
    /* Hardware reset via RST pin (if connected) */
    if (LCD_RST >= 0) {
        gpio_set_direction(LCD_RST, GPIO_MODE_OUTPUT);
        gpio_set_level(LCD_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    ili_cmd(ILI_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    ili_cmd(ILI_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Power control */
    ili_cmd(ILI_PWCTR1, (uint8_t[]){0x23}, 1);              /* VRH=4.60V */
    ili_cmd(ILI_PWCTR2, (uint8_t[]){0x10}, 1);              /* SAP, BT */
    ili_cmd(ILI_VMCTR1, (uint8_t[]){0x3E, 0x28}, 2);        /* VCOMH=4.250V, VCOML=-1.500V */
    ili_cmd(ILI_VMCTR2, (uint8_t[]){0x86}, 1);              /* VCOM offset */

    /* Pixel format: 16 bpp (RGB565) */
    ili_cmd(ILI_COLMOD, (uint8_t[]){0x55}, 1);

    /* Frame rate: 79 Hz */
    ili_cmd(ILI_FRMCTR1, (uint8_t[]){0x00, 0x18}, 2);

    /* Display function control */
    ili_cmd(ILI_DFUNCTR, (uint8_t[]){0x08, 0x82, 0x27}, 3);

    /* Memory access control (landscape + BGR) */
    ili_cmd(ILI_MADCTL, (uint8_t[]){LCD_MADCTL}, 1);

    /* Gamma correction (standard ILI9341 values) */
    ili_cmd(ILI_GMCTRP1, (uint8_t[]){
        0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
        0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00
    }, 15);
    ili_cmd(ILI_GMCTRN1, (uint8_t[]){
        0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
        0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F
    }, 15);

    ili_cmd(ILI_INVOFF, NULL, 0);
    ili_cmd(ILI_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* ── ST7789 initialisation sequence ────────────────────────────────────── */
static void st7789_init(void)
{
    if (LCD_RST >= 0) {
        gpio_set_direction(LCD_RST, GPIO_MODE_OUTPUT);
        gpio_set_level(LCD_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    ili_cmd(ILI_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    ili_cmd(ILI_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    ili_cmd(ILI_COLMOD, (uint8_t[]){0x55}, 1);          /* 16bpp RGB565 */
    ili_cmd(ILI_MADCTL, (uint8_t[]){LCD_MADCTL}, 1);
    ili_cmd(ILI_INVOFF, NULL, 0);
    ili_cmd(ILI_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void display_set_backlight(int pct)
{
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    uint32_t duty = (uint32_t)(pct * 255) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_BL_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_BL_CHANNEL);
}

void display_init(void)
{
    s_lcd_mutex = xSemaphoreCreateMutex();

    /* Backlight via LEDC PWM — OFF during init */
    ledc_timer_config_t bl_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_BL_RESOLUTION,
        .timer_num       = LEDC_BL_TIMER,
        .freq_hz         = LEDC_BASE_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&bl_timer);
    ledc_channel_config_t bl_ch = {
        .gpio_num   = LCD_BACK_LIGHT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_BL_CHANNEL,
        .timer_sel  = LEDC_BL_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&bl_ch);

    /* SPI bus */
    spi_bus_config_t buscfg = {
        .mosi_io_num   = LCD_MOSI,
        .miso_io_num   = -1,
        .sclk_io_num   = LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * STRIPE_H * 2,
    };
    spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);

    /* LCD panel IO */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = LCD_DC,
        .cs_gpio_num       = LCD_CS,
        .pclk_hz           = 40 * 1000 * 1000,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io);

#ifdef CONFIG_LCD_ST7789
    st7789_init();
#else
    ili9341_init();
#endif

    build_font16x16();

    /* Clear to black, stripe by stripe */
    memset(s_fb, 0, sizeof(s_fb));
    for (s_sy0 = 0; s_sy0 < LCD_H; s_sy0 += STRIPE_H) {
        int rows = STRIPE_H;
        if (s_sy0 + rows > LCD_H) rows = LCD_H - s_sy0;
        flush_stripe(rows);
    }
    s_sy0 = 0;

    /* Backlight ON at 100% */
    display_set_backlight(100);
}

/* ── Simple status screen shown during boot / WiFi connect ──────────────── */
void display_status(const char *line1, const char *line2)
{
    for (s_sy0 = 0; s_sy0 < LCD_H; s_sy0 += STRIPE_H) {
        int rows = STRIPE_H;
        if (s_sy0 + rows > LCD_H) rows = LCD_H - s_sy0;
        memset(s_fb, 0, LCD_W * rows * 2);

        /* Header bar */
        fill_rect(0, 0, LCD_W, 24, C_NAVY);
        draw_str(4, 8, "EleRelay", C_WHITE, C_NAVY, 1);

        fill_rect(0, 24, LCD_W, 1, C_DKGRAY);

        /* line1 — main status, scale 2 (16 px tall), vertically centred */
        if (line1) {
            int len = (int)strlen(line1);
            int x = (LCD_W - len * 16) / 2;
            if (x < 4) x = 4;
            draw_str(x, 90, line1, C_WHITE, C_BLACK, 2);
        }

        /* line2 — sub-status, scale 1, below line1 */
        if (line2) {
            int len = (int)strlen(line2);
            int x = (LCD_W - len * 8) / 2;
            if (x < 4) x = 4;
            draw_str(x, 116, line2, C_GRAY, C_BLACK, 1);
        }

        flush_stripe(rows);
    }
    s_sy0 = 0;
}

/* ── Render scene into current stripe (all draw calls self-clip) ─────────── */
static void render_scene(bool relay_on, bool ap_mode, const char *ssid, bool inet_ok,
                         time_t now, const disp_slot_t *slots, int count,
                         int cur_idx, int cheap_hours, int hours_window, int win_offset)
{
    /* ── Header bar (y 0..23, dark blue BG) ─────────────────────────── */
    fill_rect(0, 0, LCD_W, 24, C_NAVY);

    if (now > 0) {
        struct tm lt;
        localtime_r(&now, &lt);
        char tbuf[9];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
        draw_str(LCD_W - 8 * 16 - 4, 4, tbuf, C_YELLOW, C_NAVY, 2);

        char dbuf[11]; /* "DD.MM.YYYY\0" */
        int day  = lt.tm_mday;
        int mon  = lt.tm_mon + 1;
        int year = (lt.tm_year + 1900) % 10000;
        dbuf[0] = '0' + (day  / 10) % 10;
        dbuf[1] = '0' +  day        % 10;
        dbuf[2] = '.';
        dbuf[3] = '0' + (mon  / 10) % 10;
        dbuf[4] = '0' +  mon        % 10;
        dbuf[5] = '.';
        dbuf[6] = '0' + (year / 1000) % 10;
        dbuf[7] = '0' + (year /  100) % 10;
        dbuf[8] = '0' + (year /   10) % 10;
        dbuf[9] = '0' +  year         % 10;
        dbuf[10] = '\0';
        draw_str(4, 4, dbuf, C_YELLOW, C_NAVY, 2);
    }

    /* ── Separator ───────────────────────────────────────────────────── */
    fill_rect(0, 24, LCD_W, 1, C_DKGRAY);

    /* ── Relay status (y 26..99) ─────────────────────────────────────── */
    /* col1=4 (labels), col2=148 (values) — both lines share same x stops */
    const int col2 = 200;
    const char *relay_str = relay_on ? "ON " : "OFF";
    uint16_t relay_col    = relay_on ? C_GREEN : C_RED;
    draw_str(4,    34, DT("RELAY", "RELEE"), C_WHITE,    C_BLACK, 3);
    draw_str(col2, 34, relay_str,            relay_col,  C_BLACK, 3);

    /* Line 2: HIND (col1) + 0.0s (col2) + s/kWh smaller */
    if (count > 0 && cur_idx >= 0 && cur_idx < count) {
        const disp_slot_t *cur = &slots[cur_idx];
        float price_c = cur->price_eur_mwh / 10.0f;
        char pbuf[12];
        snprintf(pbuf, sizeof(pbuf), "%.1f", price_c);
        draw_str(4,    67, DT("PRICE", "HIND"), C_WHITE, C_BLACK, 3);
        draw_str(col2, 67, pbuf,                relay_col, C_BLACK, 3);
        int unit_x = col2 + (int)strlen(pbuf) * 8 * 3;
        draw_str(unit_x, 67, "s/",    C_WHITE, C_BLACK, 1);
        draw_str(unit_x, 83, "kWh",  C_WHITE, C_BLACK, 1);
    }

    /* ── Reserve gap (between labels and values) for offline "!" icon ──
     * display_blink_offline() draws/clears it directly at 3Hz when offline. */
    if (!ap_mode && !inet_ok) {
        fill_rect(126, 26, 74, 73, C_BLACK);
    }

    /* ── WiFi / AP info + window (y 102..121) ───────────────────────────── */
    fill_rect(0, 100, LCD_W, 1, C_DKGRAY);
    /* Hours setting right-aligned: e.g. "6h/12h" */
    char hbuf[12];
    snprintf(hbuf, sizeof(hbuf), "%dh/%dh", cheap_hours, hours_window);
    int hx = LCD_W - (int)strlen(hbuf) * 8 - 4;
    draw_str(hx, 107, hbuf, C_YELLOW, C_BLACK, 1);
    if (ap_mode) {
        draw_str(4, 107, "AP: EleRelay-Setup  192.168.4.1", C_ORANGE, C_BLACK, 1);
    } else if (!inet_ok) {
        draw_str(4, 107, DT("NO INTERNET CONNECTION", "INTERNETI UHENDUS PUUDUB"),
                 C_RED, C_BLACK, 1);
    } else {
        char wbuf[48];
        snprintf(wbuf, sizeof(wbuf), "WiFi: %s", ssid ? ssid : "");
        draw_str(4, 107, wbuf, C_CYAN, C_BLACK, 1);
    }

    /* ── Price bar chart (y 124..239) ────────────────────────────────── */
    fill_rect(0, 122, LCD_W, 1, C_DKGRAY);

    int bars = count < DISP_MAX_BARS ? count : DISP_MAX_BARS;
    if (bars > 0) {
        float pmin =  1e9f, pmax = -1e9f;
        for (int i = 0; i < bars; i++) {
            float p = slots[i].price_eur_mwh;
            if (p < pmin) pmin = p;
            if (p > pmax) pmax = p;
        }
        float prange = pmax - pmin;
        if (prange < 1.0f) prange = 1.0f;

        int chart_y = 124;
        int chart_h = LCD_H - chart_y - 1;  /* ~115 px */
        int bar_w   = LCD_W / bars;
        if (bar_w < 1) bar_w = 1;

        for (int i = 0; i < bars; i++) {
            float p   = slots[i].price_eur_mwh;
            int bar_h = (int)((p - pmin) / prange * (chart_h - 2)) + 2;
            if (bar_h > chart_h) bar_h = chart_h;
            int bx = i * bar_w;
            int by = chart_y + chart_h - bar_h;
            uint16_t col = slots[i].is_on ? C_DKGREEN : C_DKRED;
            if (i == cur_idx) col = slots[i].is_on ? C_GREEN : C_RED;
            fill_rect(bx, by, bar_w - 1, bar_h, col);
        }

        /* 1px vertical separators at every hours_window boundary (4 slots/h) */
        if (hours_window > 0) {
            int slots_per_window = hours_window * 4;
            /* first_sep: how many bars until the next true window boundary */
            int first_sep = (win_offset == 0) ? slots_per_window
                                              : (slots_per_window - win_offset);
            for (int i = first_sep; i < bars; i += slots_per_window) {
                int sx = i * bar_w;
                fill_rect(sx, chart_y, 1, chart_h, C_WHITE);
            }
        }
    }
}

/* ── Config page ─────────────────────────────────────────────────────────── */

/*
 * Layout arithmetic (screen 320×240, header+sep = 25px, save = 32px, available = 182px):
 *
 * Page 1 — 4 items: 3×(lbl8+btn26)=102 + save32 = 134px
 *   5 gaps of 16px = 80px, +1px bottom → all gaps ~equal
 *   item_y = 25 + 16 + cumulative
 *
 * Page 2 — 2 sections × (lbl16+btn26+tog22) + 1px sep = 129px content
 *   53px for 8 gaps: 8,6,6,7 | sep | 6,6,6,8 → all gaps ~equal
 *   Order per section: label → price (-/+) → toggle
 */
#define CFG_BTN_W      60
#define CFG_BTN_H      26
#define CFG_TOG_H      22
#define CFG_TOG_X0     20
#define CFG_TOG_X1    299
#define CFG_DEC_X      20
#define CFG_INC_X     240
#define CFG_SAVE_X1    60
#define CFG_SAVE_X2   259
#define CFG_SAVE_H     32
#define CFG_CLOSE_X1  282
/* Page navigation buttons (bottom row, flanking Save) */
#define CFG_PREV_BTN_X   0   /* [<] x=0..59  */
#define CFG_NEXT_BTN_X 260   /* [>] x=260..319 */
#define CFG_NAV_BTN_W   60
/* Page 1: label at (btn_y - 8), gap=16px between items */
#define CFG_WIN_Y      55   /* label@39(scale2) btn@55 end@81  */
#define CFG_CHE_Y     111   /* label@95(scale2) btn@111 end@137 */
#define CFG_MIN_Y     167   /* label@151(scale2) btn@167 end@193 */
#define CFG1_SAVE_Y   208   /* pinned to LCD bottom: 240-32=208 */
/* Page 2: label→price→toggle order; gaps 8,6,6,7|sep|6,6,6,8 */
#define CFG2_AON_LBL_Y   34  /* scale2 label@34  end@50  */
#define CFG2_AON_PRC_Y   56  /* price btn@56     end@82  */
#define CFG2_AON_TOG_Y   88  /* toggle@88        end@110 */
#define CFG2_SEP_Y      117  /* 1px separator            */
#define CFG2_AOFF_LBL_Y 124  /* scale2 label@124 end@140 */
#define CFG2_AOFF_PRC_Y 146  /* price btn@146    end@172 */
#define CFG2_AOFF_TOG_Y 178  /* toggle@178       end@200 */
#define CFG2_SAVE_Y     208  /* pinned to LCD bottom: 240-32=208 */

static void draw_btn(int x, int y, int w, int h, const char *text, uint16_t bg)
{
    fill_rect(x, y, w, h, bg);
    fill_rect(x, y, w, 1, C_GRAY);
    fill_rect(x, y + h - 1, w, 1, C_GRAY);
    fill_rect(x, y, 1, h, C_GRAY);
    fill_rect(x + w - 1, y, 1, h, C_GRAY);
    int tw = (int)strlen(text) * 8 * 2;
    draw_str(x + (w - tw) / 2, y + (h - 16) / 2, text, C_WHITE, bg, 2);
}

static void render_config_page1(int cheap_hours, int hours_window, int min_run_minutes)
{
    /* Header */
    fill_rect(0, 0, LCD_W, 24, C_NAVY);
    draw_str(4, 8, DT("Config 1/4", "Seaded 1/4"), C_WHITE, C_NAVY, 1);
    fill_rect(CFG_CLOSE_X1, 0, LCD_W - CFG_CLOSE_X1, 24, C_DKRED);
    draw_str(CFG_CLOSE_X1 + (LCD_W - CFG_CLOSE_X1 - 8) / 2, 8, "X", C_WHITE, C_DKRED, 1);
    fill_rect(0, 24, LCD_W, 1, C_DKGRAY);

    /* Window size row */
    draw_str(4, CFG_WIN_Y - 16, DT("Window size:", "Akna suurus:"), C_YELLOW, C_BLACK, 2);
    draw_btn(CFG_DEC_X, CFG_WIN_Y, CFG_BTN_W, CFG_BTN_H, "-", C_DKGRAY);
    draw_btn(CFG_INC_X, CFG_WIN_Y, CFG_BTN_W, CFG_BTN_H, "+", C_DKGRAY);
    char vbuf[16];
    snprintf(vbuf, sizeof(vbuf), "%dh", hours_window);
    {
        int vw = (int)strlen(vbuf) * 16;
        int vx = (CFG_DEC_X + CFG_BTN_W + CFG_INC_X) / 2 - vw / 2;
        draw_str(vx, CFG_WIN_Y + (CFG_BTN_H - 16) / 2, vbuf, C_WHITE, C_BLACK, 2);
    }

    /* Cheap hours row */
    draw_str(4, CFG_CHE_Y - 16, DT("Cheap hours:", "Odavad tunnid:"), C_YELLOW, C_BLACK, 2);
    draw_btn(CFG_DEC_X, CFG_CHE_Y, CFG_BTN_W, CFG_BTN_H, "-", C_DKGRAY);
    draw_btn(CFG_INC_X, CFG_CHE_Y, CFG_BTN_W, CFG_BTN_H, "+", C_DKGRAY);
    snprintf(vbuf, sizeof(vbuf), "%dh", cheap_hours);
    {
        int vw = (int)strlen(vbuf) * 16;
        int vx = (CFG_DEC_X + CFG_BTN_W + CFG_INC_X) / 2 - vw / 2;
        draw_str(vx, CFG_CHE_Y + (CFG_BTN_H - 16) / 2, vbuf, C_WHITE, C_BLACK, 2);
    }

    /* Min. run time row */
    draw_str(4, CFG_MIN_Y - 16, DT("Min. run time:", "Min. kaitusaeg:"), C_YELLOW, C_BLACK, 2);
    draw_btn(CFG_DEC_X, CFG_MIN_Y, CFG_BTN_W, CFG_BTN_H, "-", C_DKGRAY);
    draw_btn(CFG_INC_X, CFG_MIN_Y, CFG_BTN_W, CFG_BTN_H, "+", C_DKGRAY);
    if (min_run_minutes == 0) {
        snprintf(vbuf, sizeof(vbuf), "OFF");
    } else {
        snprintf(vbuf, sizeof(vbuf), "%dm", min_run_minutes);
    }
    {
        int vw = (int)strlen(vbuf) * 16;
        int vx = (CFG_DEC_X + CFG_BTN_W + CFG_INC_X) / 2 - vw / 2;
        draw_str(vx, CFG_MIN_Y + (CFG_BTN_H - 16) / 2, vbuf, C_WHITE, C_BLACK, 2);
    }

    /* Bottom row: [save] + [>] next page */
    draw_btn(CFG_SAVE_X1, CFG1_SAVE_Y, CFG_SAVE_X2 - CFG_SAVE_X1, CFG_SAVE_H,
             DT("SAVE", "SALVESTA"), C_DKGREEN);
    draw_btn(CFG_NEXT_BTN_X, CFG1_SAVE_Y, CFG_NAV_BTN_W, CFG_SAVE_H, ">", C_DKGRAY);
}

static void render_config_page2(bool aon_en, int aon_lim_mwh,
                                 bool aoff_en, int aoff_lim_mwh)
{
    /* Header */
    fill_rect(0, 0, LCD_W, 24, C_NAVY);
    draw_str(4, 8, DT("Config 2/4", "Seaded 2/4"), C_WHITE, C_NAVY, 1);
    fill_rect(CFG_CLOSE_X1, 0, LCD_W - CFG_CLOSE_X1, 24, C_DKRED);
    draw_str(CFG_CLOSE_X1 + (LCD_W - CFG_CLOSE_X1 - 8) / 2, 8, "X", C_WHITE, C_DKRED, 1);
    fill_rect(0, 24, LCD_W, 1, C_DKGRAY);

    /* Always ON section: label → price → toggle */
    draw_str(4, CFG2_AON_LBL_Y, DT("Always ON below:", "Alati sees alla:"), C_YELLOW, C_BLACK, 2);
    draw_btn(CFG_DEC_X, CFG2_AON_PRC_Y, CFG_BTN_W, CFG_BTN_H, "-", C_DKGRAY);
    draw_btn(CFG_INC_X, CFG2_AON_PRC_Y, CFG_BTN_W, CFG_BTN_H, "+", C_DKGRAY);
    {
        char pvbuf[10];
        snprintf(pvbuf, sizeof(pvbuf), "%.1f", aon_lim_mwh / 10.0f);
        int vw = (int)strlen(pvbuf) * 16;
        int cx = (CFG_DEC_X + CFG_BTN_W + CFG_INC_X) / 2;
        int vx = cx - vw / 2;
        int vy = CFG2_AON_PRC_Y + (CFG_BTN_H - 16) / 2;
        draw_str(vx, vy, pvbuf, C_WHITE, C_BLACK, 2);
        draw_str(cx + vw / 2 + 2, vy + 4, "s/kWh", C_GRAY, C_BLACK, 1);
    }
    {
        uint16_t tcol = aon_en ? C_DKGREEN : C_DKGRAY;
        draw_btn(CFG_TOG_X0, CFG2_AON_TOG_Y, CFG_TOG_X1 - CFG_TOG_X0, CFG_TOG_H,
                 aon_en ? DT("ENABLED", "SEES") : DT("DISABLED", "VALJAS"), tcol);
    }

    /* Section separator */
    fill_rect(0, CFG2_SEP_Y, LCD_W, 1, C_DKGRAY);

    /* Always OFF section: label → price → toggle */
    draw_str(4, CFG2_AOFF_LBL_Y, DT("Always OFF above:", "Alati valjas yle:"), C_YELLOW, C_BLACK, 2);
    draw_btn(CFG_DEC_X, CFG2_AOFF_PRC_Y, CFG_BTN_W, CFG_BTN_H, "-", C_DKGRAY);
    draw_btn(CFG_INC_X, CFG2_AOFF_PRC_Y, CFG_BTN_W, CFG_BTN_H, "+", C_DKGRAY);
    {
        char pvbuf[10];
        snprintf(pvbuf, sizeof(pvbuf), "%.1f", aoff_lim_mwh / 10.0f);
        int vw = (int)strlen(pvbuf) * 16;
        int cx = (CFG_DEC_X + CFG_BTN_W + CFG_INC_X) / 2;
        int vx = cx - vw / 2;
        int vy = CFG2_AOFF_PRC_Y + (CFG_BTN_H - 16) / 2;
        draw_str(vx, vy, pvbuf, C_WHITE, C_BLACK, 2);
        draw_str(cx + vw / 2 + 2, vy + 4, "s/kWh", C_GRAY, C_BLACK, 1);
    }
    {
        uint16_t tcol = aoff_en ? C_DKGREEN : C_DKGRAY;
        draw_btn(CFG_TOG_X0, CFG2_AOFF_TOG_Y, CFG_TOG_X1 - CFG_TOG_X0, CFG_TOG_H,
                 aoff_en ? DT("ENABLED", "SEES") : DT("DISABLED", "VALJAS"), tcol);
    }

    /* Bottom row: [<] + [save] + [>] */
    draw_btn(CFG_PREV_BTN_X, CFG2_SAVE_Y, CFG_NAV_BTN_W, CFG_SAVE_H, "<", C_DKGRAY);
    draw_btn(CFG_SAVE_X1, CFG2_SAVE_Y, CFG_SAVE_X2 - CFG_SAVE_X1, CFG_SAVE_H,
             DT("SAVE", "SALVESTA"), C_DKGREEN);
    draw_btn(CFG_NEXT_BTN_X, CFG2_SAVE_Y, CFG_NAV_BTN_W, CFG_SAVE_H, ">", C_DKGRAY);
}

/* Page 3 — backlight brightness */
#define CFG3_BL_Y   110   /* [-] value [+] row Y */

static void render_config_page3(int backlight_pct)
{
    /* Header */
    fill_rect(0, 0, LCD_W, 24, C_NAVY);
    draw_str(4, 8, DT("Config 3/4", "Seaded 3/4"), C_WHITE, C_NAVY, 1);
    fill_rect(CFG_CLOSE_X1, 0, LCD_W - CFG_CLOSE_X1, 24, C_DKRED);
    draw_str(CFG_CLOSE_X1 + (LCD_W - CFG_CLOSE_X1 - 8) / 2, 8, "X", C_WHITE, C_DKRED, 1);
    fill_rect(0, 24, LCD_W, 1, C_DKGRAY);

    /* Backlight row */
    draw_str(4, CFG3_BL_Y - 16, DT("Backlight:", "Taustvalgus:"), C_YELLOW, C_BLACK, 2);
    draw_btn(CFG_DEC_X, CFG3_BL_Y, CFG_BTN_W, CFG_BTN_H, "-", C_DKGRAY);
    draw_btn(CFG_INC_X, CFG3_BL_Y, CFG_BTN_W, CFG_BTN_H, "+", C_DKGRAY);
    char vbuf[8];
    snprintf(vbuf, sizeof(vbuf), "%d%%", backlight_pct);
    {
        int vw = (int)strlen(vbuf) * 16;
        int vx = (CFG_DEC_X + CFG_BTN_W + CFG_INC_X) / 2 - vw / 2;
        draw_str(vx, CFG3_BL_Y + (CFG_BTN_H - 16) / 2, vbuf, C_WHITE, C_BLACK, 2);
    }

    /* Bottom row: [<] + [save] + [>] */
    draw_btn(CFG_PREV_BTN_X, CFG1_SAVE_Y, CFG_NAV_BTN_W, CFG_SAVE_H, "<", C_DKGRAY);
    draw_btn(CFG_SAVE_X1, CFG1_SAVE_Y, CFG_SAVE_X2 - CFG_SAVE_X1, CFG_SAVE_H,
             DT("SAVE", "SALVESTA"), C_DKGREEN);
    draw_btn(CFG_NEXT_BTN_X, CFG1_SAVE_Y, CFG_NAV_BTN_W, CFG_SAVE_H, ">", C_DKGRAY);
}

/* Page 4 — system info */
static void render_config_page4(const disp_sysinfo_t *info)
{
    char buf[48];

    /* Header */
    fill_rect(0, 0, LCD_W, 24, C_NAVY);
    draw_str(4, 8, DT("Config 4/4", "Seaded 4/4"), C_WHITE, C_NAVY, 1);
    fill_rect(CFG_CLOSE_X1, 0, LCD_W - CFG_CLOSE_X1, 24, C_DKRED);
    draw_str(CFG_CLOSE_X1 + (LCD_W - CFG_CLOSE_X1 - 8) / 2, 8, "X", C_WHITE, C_DKRED, 1);
    fill_rect(0, 24, LCD_W, 1, C_DKGRAY);

    /* ── App info block ── */
    {
        const char *name = "EleRelay";
        int nx = (LCD_W - (int)strlen(name) * 16) / 2;
        draw_str(nx, 31, name, C_CYAN, C_BLACK, 2);
    }
    if (info->app_ver && *info->app_ver) {
        snprintf(buf, sizeof(buf), "v%s", info->app_ver);
        int vx = (LCD_W - (int)strlen(buf) * 8) / 2;
        draw_str(vx, 51, buf, C_WHITE, C_BLACK, 1);
    }
    if (info->build_date && info->build_time) {
        snprintf(buf, sizeof(buf), "Built: %s %s", info->build_date, info->build_time);
        int dx = (LCD_W - (int)strlen(buf) * 8) / 2;
        draw_str(dx, 63, buf, C_GRAY, C_BLACK, 1);
    }

    /* ── WiFi block ── */
    fill_rect(0, 76, LCD_W, 1, C_DKGRAY);

#define LBL_X  4
#define VAL_X  60
#define ROW(y, lbl, val, col) \
    draw_str(LBL_X, y, lbl, C_YELLOW, C_BLACK, 1); \
    draw_str(VAL_X, y, val, col,      C_BLACK, 1)

    snprintf(buf, sizeof(buf), "%s", (info->ssid && *info->ssid) ? info->ssid : "(AP mode)");
    ROW(82, DT("WiFi:", "WiFi:"), buf, C_CYAN);

    snprintf(buf, sizeof(buf), "%s", info->ip ? info->ip : "");
    ROW(94, "IP:", buf, C_WHITE);

    ROW(106, "MAC:", info->mac, C_WHITE);

    ROW(118, DT("Host:", "Host:"), info->hostname, C_WHITE);

    if (info->channel > 0) {
        int quality = (int)(2 * ((int)info->rssi + 100));
        if (quality < 0)   quality = 0;
        if (quality > 100) quality = 100;
        snprintf(buf, sizeof(buf), "%ddBm  Ch:%d  %d%%",
                 (int)info->rssi, (int)info->channel, quality);
    } else {
        snprintf(buf, sizeof(buf), "N/A");
    }
    ROW(130, "RSSI:", buf, C_WHITE);

    /* ── System block ── */
    fill_rect(0, 144, LCD_W, 1, C_DKGRAY);

    snprintf(buf, sizeof(buf), "%luKB free", (unsigned long)(info->free_heap / 1024));
    ROW(151, DT("Heap:", "Heap:"), buf, C_WHITE);

    snprintf(buf, sizeof(buf), "%luMHz", (unsigned long)info->cpu_mhz);
    ROW(163, "CPU:", buf, C_WHITE);

    {
        uint32_t u = info->uptime_sec;
        uint32_t d = u / 86400; u %= 86400;
        uint32_t h = u / 3600;  u %= 3600;
        uint32_t m = u / 60;
        snprintf(buf, sizeof(buf), "%lud %02lu:%02lu", (unsigned long)d,
                 (unsigned long)h, (unsigned long)m);
    }
    ROW(175, DT("Up:", "Up:"), buf, C_WHITE);

    if (info->last_fetch > 0) {
        struct tm lt;
        localtime_r(&info->last_fetch, &lt);
        snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    } else {
        snprintf(buf, sizeof(buf), "never");
    }
    ROW(187, DT("Fetch:", "Fetch:"), buf, C_WHITE);

    ROW(199, DT("Relay:", "Relay:"), info->relay_reason, C_WHITE);

#undef ROW
#undef LBL_X
#undef VAL_X

    /* Bottom row: [<] back only */
    draw_btn(0, CFG2_SAVE_Y, LCD_W, CFG_SAVE_H, "<", C_DKGRAY);
}

void display_show_config(int cheap_hours, int hours_window, int min_run_minutes,
                          bool aon_en, int aon_lim_mwh,
                          bool aoff_en, int aoff_lim_mwh, int page,
                          int backlight_pct,
                          const disp_sysinfo_t *info)
{
    for (s_sy0 = 0; s_sy0 < LCD_H; s_sy0 += STRIPE_H) {
        int rows = STRIPE_H;
        if (s_sy0 + rows > LCD_H) rows = LCD_H - s_sy0;
        memset(s_fb, 0, LCD_W * rows * 2);
        if (page == 3)
            render_config_page4(info);
        else if (page == 2)
            render_config_page3(backlight_pct);
        else if (page == 1)
            render_config_page2(aon_en, aon_lim_mwh, aoff_en, aoff_lim_mwh);
        else
            render_config_page1(cheap_hours, hours_window, min_run_minutes);
        flush_stripe(rows);
    }
    s_sy0 = 0;
}

int display_config_hittest(int tx, int ty, int page)
{
    if (tx >= CFG_CLOSE_X1 && ty <= 23) return DISP_CFG_CLOSE;

    if (page == 0) {
        if (tx >= CFG_NEXT_BTN_X && tx < CFG_NEXT_BTN_X + CFG_NAV_BTN_W &&
            ty >= CFG1_SAVE_Y && ty < CFG1_SAVE_Y + CFG_SAVE_H)
            return DISP_CFG_NEXT_PAGE;
        if (tx >= CFG_DEC_X && tx < CFG_DEC_X + CFG_BTN_W) {
            if (ty >= CFG_WIN_Y && ty < CFG_WIN_Y + CFG_BTN_H) return DISP_CFG_WIN_DEC;
            if (ty >= CFG_CHE_Y && ty < CFG_CHE_Y + CFG_BTN_H) return DISP_CFG_CHE_DEC;
            if (ty >= CFG_MIN_Y && ty < CFG_MIN_Y + CFG_BTN_H) return DISP_CFG_MIN_DEC;
        }
        if (tx >= CFG_INC_X && tx < CFG_INC_X + CFG_BTN_W) {
            if (ty >= CFG_WIN_Y && ty < CFG_WIN_Y + CFG_BTN_H) return DISP_CFG_WIN_INC;
            if (ty >= CFG_CHE_Y && ty < CFG_CHE_Y + CFG_BTN_H) return DISP_CFG_CHE_INC;
            if (ty >= CFG_MIN_Y && ty < CFG_MIN_Y + CFG_BTN_H) return DISP_CFG_MIN_INC;
        }
        if (tx >= CFG_SAVE_X1 && tx <= CFG_SAVE_X2 &&
            ty >= CFG1_SAVE_Y && ty < CFG1_SAVE_Y + CFG_SAVE_H) return DISP_CFG_SAVE;
    } else if (page == 1) {
        if (tx >= CFG_PREV_BTN_X && tx < CFG_PREV_BTN_X + CFG_NAV_BTN_W &&
            ty >= CFG2_SAVE_Y && ty < CFG2_SAVE_Y + CFG_SAVE_H)
            return DISP_CFG_PREV_PAGE;
        if (tx >= CFG_NEXT_BTN_X && tx < CFG_NEXT_BTN_X + CFG_NAV_BTN_W &&
            ty >= CFG2_SAVE_Y && ty < CFG2_SAVE_Y + CFG_SAVE_H)
            return DISP_CFG_NEXT_PAGE;
        if (tx >= CFG_TOG_X0 && tx <= CFG_TOG_X1) {
            if (ty >= CFG2_AON_TOG_Y  && ty < CFG2_AON_TOG_Y  + CFG_TOG_H) return DISP_CFG_AON_TOG;
            if (ty >= CFG2_AOFF_TOG_Y && ty < CFG2_AOFF_TOG_Y + CFG_TOG_H) return DISP_CFG_AOFF_TOG;
        }
        if (tx >= CFG_DEC_X && tx < CFG_DEC_X + CFG_BTN_W) {
            if (ty >= CFG2_AON_PRC_Y  && ty < CFG2_AON_PRC_Y  + CFG_BTN_H) return DISP_CFG_AON_DEC;
            if (ty >= CFG2_AOFF_PRC_Y && ty < CFG2_AOFF_PRC_Y + CFG_BTN_H) return DISP_CFG_AOFF_DEC;
        }
        if (tx >= CFG_INC_X && tx < CFG_INC_X + CFG_BTN_W) {
            if (ty >= CFG2_AON_PRC_Y  && ty < CFG2_AON_PRC_Y  + CFG_BTN_H) return DISP_CFG_AON_INC;
            if (ty >= CFG2_AOFF_PRC_Y && ty < CFG2_AOFF_PRC_Y + CFG_BTN_H) return DISP_CFG_AOFF_INC;
        }
        if (tx >= CFG_SAVE_X1 && tx <= CFG_SAVE_X2 &&
            ty >= CFG2_SAVE_Y && ty < CFG2_SAVE_Y + CFG_SAVE_H) return DISP_CFG_SAVE;
    } else if (page == 2) {
        /* Backlight page */
        if (tx >= CFG_PREV_BTN_X && tx < CFG_PREV_BTN_X + CFG_NAV_BTN_W &&
            ty >= CFG1_SAVE_Y && ty < CFG1_SAVE_Y + CFG_SAVE_H)
            return DISP_CFG_PREV_PAGE;
        if (tx >= CFG_NEXT_BTN_X && tx < CFG_NEXT_BTN_X + CFG_NAV_BTN_W &&
            ty >= CFG1_SAVE_Y && ty < CFG1_SAVE_Y + CFG_SAVE_H)
            return DISP_CFG_NEXT_PAGE;
        if (tx >= CFG_DEC_X && tx < CFG_DEC_X + CFG_BTN_W &&
            ty >= CFG3_BL_Y && ty < CFG3_BL_Y + CFG_BTN_H)
            return DISP_CFG_BL_DEC;
        if (tx >= CFG_INC_X && tx < CFG_INC_X + CFG_BTN_W &&
            ty >= CFG3_BL_Y && ty < CFG3_BL_Y + CFG_BTN_H)
            return DISP_CFG_BL_INC;
        if (tx >= CFG_SAVE_X1 && tx <= CFG_SAVE_X2 &&
            ty >= CFG1_SAVE_Y && ty < CFG1_SAVE_Y + CFG_SAVE_H)
            return DISP_CFG_SAVE;
    } else { /* page 3: info only */
        if (ty >= CFG2_SAVE_Y && ty < CFG2_SAVE_Y + CFG_SAVE_H)
            return DISP_CFG_PREV_PAGE;
    }
    return -1;
}

void display_update(bool relay_on, bool ap_mode, const char *ssid, bool inet_ok,
                    time_t now, const disp_slot_t *slots, int count, int cur_idx,
                    int cheap_hours, int hours_window, int win_offset)
{
    xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
    for (s_sy0 = 0; s_sy0 < LCD_H; s_sy0 += STRIPE_H) {
        int rows = STRIPE_H;
        if (s_sy0 + rows > LCD_H) rows = LCD_H - s_sy0;

        memset(s_fb, 0, LCD_W * rows * 2);
        render_scene(relay_on, ap_mode, ssid, inet_ok, now, slots, count, cur_idx,
                     cheap_hours, hours_window, win_offset);
        flush_stripe(rows);
    }
    s_sy0 = 0;
    xSemaphoreGive(s_lcd_mutex);
}
