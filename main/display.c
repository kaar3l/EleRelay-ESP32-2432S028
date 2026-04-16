/*
 * display.c — ILI9341 display driver for ESP-2432S028 (Cheap Yellow Display)
 *
 * Landscape 320×240, RGB565.
 * Layout:
 *   y=  0..23  header: "EleRelay" + HH:MM + date
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
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

/* ── ESP-2432S028 pin map ───────────────────────────────────────────────── */
#define LCD_MOSI   13
#define LCD_SCLK   14
#define LCD_CS     15
#define LCD_DC      2
#define LCD_RST    -1   /* Not connected; rely on software reset */
#define LCD_BL     21

#define LCD_W     320
#define LCD_H     240
#define LCD_HOST  SPI2_HOST

/* ── ILI9341 command codes ──────────────────────────────────────────────── */
#define ILI_SWRESET  0x01
#define ILI_SLPOUT   0x11
#define ILI_INVOFF   0x20
#define ILI_DISPON   0x29
#define ILI_CASET    0x2A
#define ILI_RASET    0x2B
#define ILI_RAMWR    0x2C
#define ILI_MADCTL   0x36
#define ILI_COLMOD   0x3A
#define ILI_FRMCTR1  0xB1
#define ILI_DFUNCTR  0xB6
#define ILI_PWCTR1   0xC0
#define ILI_PWCTR2   0xC1
#define ILI_VMCTR1   0xC5
#define ILI_VMCTR2   0xC7
#define ILI_GMCTRP1  0xE0
#define ILI_GMCTRN1  0xE1

/*
 * MADCTL for landscape 320×240 on ESP-2432S028:
 *   MV=1 (bit5) — swap row/column → landscape
 *   BGR=1 (bit3) — ILI9341 native color order
 * Other orientations to try if display is wrong:
 *   0x68 (MX=1, MV=1, BGR=1) — X-mirrored landscape
 *   0xA8 (MY=1, MV=1, BGR=1) — Y-mirrored landscape
 *   0xE8 (MY=1, MX=1, MV=1, BGR=1) — 180° rotated landscape
 */
#define LCD_MADCTL   0x28

/* ── RGB565 colour palette ──────────────────────────────────────────────── */
#define C_BLACK   0x0000u
#define C_WHITE   0xFFFFu
#define C_GREEN   0x07E0u
#define C_DKGREEN 0x03E0u
#define C_RED     0xF800u
#define C_DKRED   0x7800u
#define C_BLUE    0x001Fu
#define C_NAVY    0x000Fu
#define C_YELLOW  0xFFE0u
#define C_ORANGE  0xFD20u
#define C_GRAY    0x7BEFu
#define C_DKGRAY  0x39E7u
#define C_CYAN    0x07FFu

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

/* ── Globals ────────────────────────────────────────────────────────────── */
static esp_lcd_panel_io_handle_t s_io = NULL;

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

    ili_cmd(ILI_INVOFF, NULL, 0);  /* No inversion for ILI9341 on CYD */
    ili_cmd(ILI_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void display_init(void)
{
    /* Backlight OFF during init */
    gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL, 0);

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

    ili9341_init();

    /* Clear to black, stripe by stripe */
    memset(s_fb, 0, sizeof(s_fb));
    for (s_sy0 = 0; s_sy0 < LCD_H; s_sy0 += STRIPE_H) {
        int rows = STRIPE_H;
        if (s_sy0 + rows > LCD_H) rows = LCD_H - s_sy0;
        flush_stripe(rows);
    }
    s_sy0 = 0;

    /* Backlight ON */
    gpio_set_level(LCD_BL, 1);
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
static void render_scene(bool relay_on, bool ap_mode, const char *ssid,
                         time_t now, const disp_slot_t *slots, int count,
                         int cur_idx, int cheap_hours, int hours_window)
{
    /* ── Header bar (y 0..23, dark blue BG) ─────────────────────────── */
    fill_rect(0, 0, LCD_W, 24, C_NAVY);
    draw_str(4, 0, "EleRelay", C_WHITE, C_NAVY, 1);

    if (now > 0) {
        struct tm lt;
        localtime_r(&now, &lt);
        char tbuf[8];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", lt.tm_hour, lt.tm_min);
        draw_str(LCD_W - 5 * 16 - 4, 4, tbuf, C_WHITE, C_NAVY, 2);

        static const char *days[]   = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
        char dbuf[16];
        snprintf(dbuf, sizeof(dbuf), "%s %d %s",
                 days[lt.tm_wday], lt.tm_mday, months[lt.tm_mon]);
        int dw = (int)strlen(dbuf) * 8;
        draw_str(LCD_W / 2 - dw / 2, 8, dbuf, C_GRAY, C_NAVY, 1);
    }

    /* ── Separator ───────────────────────────────────────────────────── */
    fill_rect(0, 24, LCD_W, 1, C_DKGRAY);

    /* ── Relay status (y 26..99, large text) ────────────────────────── */
    const char *relay_str = relay_on ? "ON " : "OFF";
    uint16_t relay_col    = relay_on ? C_GREEN : C_RED;
    draw_str(4, 28, "RELAY", C_GRAY, C_BLACK, 2);
    draw_str(4, 52, relay_str, relay_col, C_BLACK, 4);  /* 32 px tall */

    /* ── Current price (right half of relay area) ────────────────────── */
    if (count > 0 && cur_idx >= 0 && cur_idx < count) {
        const disp_slot_t *cur = &slots[cur_idx];
        float price_c = cur->price_eur_mwh / 10.0f;  /* EUR/MWh → c/kWh */
        char pbuf[20];
        snprintf(pbuf, sizeof(pbuf), "%.1fc", price_c);
        uint16_t pc = cur->is_cheap ? C_GREEN : C_RED;
        draw_str(176, 28, cur->is_cheap ? "CHEAP" : "EXPNS", pc, C_BLACK, 2);
        draw_str(176, 52, pbuf, C_WHITE, C_BLACK, 3);
        draw_str(176, 84, "/kWh", C_GRAY, C_BLACK, 1);
    }

    /* ── WiFi / AP info + window (y 102..121) ───────────────────────────── */
    fill_rect(0, 100, LCD_W, 1, C_DKGRAY);
    /* Hours setting right-aligned: e.g. "6h/12h" */
    char hbuf[12];
    snprintf(hbuf, sizeof(hbuf), "%dh/%dh", cheap_hours, hours_window);
    int hx = LCD_W - (int)strlen(hbuf) * 8 - 4;
    draw_str(hx, 104, hbuf, C_YELLOW, C_BLACK, 1);
    if (ap_mode) {
        draw_str(4, 104, "AP: EleRelay-Setup  192.168.4.1", C_ORANGE, C_BLACK, 1);
    } else {
        char wbuf[48];
        snprintf(wbuf, sizeof(wbuf), "WiFi: %s", ssid ? ssid : "");
        draw_str(4, 104, wbuf, C_CYAN, C_BLACK, 1);
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
            uint16_t col = slots[i].is_cheap ? C_DKGREEN : C_DKRED;
            if (i == cur_idx) col = slots[i].is_cheap ? C_GREEN : C_RED;
            fill_rect(bx, by, bar_w - 1, bar_h, col);
        }
    }
}

void display_update(bool relay_on, bool ap_mode, const char *ssid,
                    time_t now, const disp_slot_t *slots, int count, int cur_idx,
                    int cheap_hours, int hours_window)
{
    for (s_sy0 = 0; s_sy0 < LCD_H; s_sy0 += STRIPE_H) {
        int rows = STRIPE_H;
        if (s_sy0 + rows > LCD_H) rows = LCD_H - s_sy0;

        memset(s_fb, 0, LCD_W * rows * 2);
        render_scene(relay_on, ap_mode, ssid, now, slots, count, cur_idx,
                     cheap_hours, hours_window);
        flush_stripe(rows);
    }
    s_sy0 = 0;
}
