#include "display.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "logo_assets.h"

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 240
#define LCD_V_RES 135
#define LCD_PIXELS (LCD_H_RES * LCD_V_RES)

#ifndef CONFIG_PSWAKE_HAS_DISPLAY
#define CONFIG_PSWAKE_HAS_DISPLAY 0
#endif

#ifndef CONFIG_PSWAKE_LCD_SWAP_XY
#define CONFIG_PSWAKE_LCD_SWAP_XY 0
#endif

#ifndef CONFIG_PSWAKE_LCD_MIRROR_X
#define CONFIG_PSWAKE_LCD_MIRROR_X 0
#endif

#ifndef CONFIG_PSWAKE_LCD_MIRROR_Y
#define CONFIG_PSWAKE_LCD_MIRROR_Y 0
#endif

#ifndef CONFIG_PSWAKE_LCD_X_GAP
#define CONFIG_PSWAKE_LCD_X_GAP 0
#endif

#ifndef CONFIG_PSWAKE_LCD_Y_GAP
#define CONFIG_PSWAKE_LCD_Y_GAP 0
#endif

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static display_os_t s_os = DISPLAY_OS_PS5;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}

static const uint8_t font[][6] = {
    {' ', 0x00, 0x00, 0x00, 0x00, 0x00}, {'-', 0x08, 0x08, 0x08, 0x08, 0x08},
    {'.', 0x00, 0x60, 0x60, 0x00, 0x00}, {':', 0x00, 0x36, 0x36, 0x00, 0x00},
    {'0', 0x3e, 0x51, 0x49, 0x45, 0x3e}, {'1', 0x00, 0x42, 0x7f, 0x40, 0x00},
    {'2', 0x42, 0x61, 0x51, 0x49, 0x46}, {'3', 0x21, 0x41, 0x45, 0x4b, 0x31},
    {'4', 0x18, 0x14, 0x12, 0x7f, 0x10}, {'5', 0x27, 0x45, 0x45, 0x45, 0x39},
    {'6', 0x3c, 0x4a, 0x49, 0x49, 0x30}, {'7', 0x01, 0x71, 0x09, 0x05, 0x03},
    {'8', 0x36, 0x49, 0x49, 0x49, 0x36}, {'9', 0x06, 0x49, 0x49, 0x29, 0x1e},
    {'A', 0x7e, 0x11, 0x11, 0x11, 0x7e}, {'B', 0x7f, 0x49, 0x49, 0x49, 0x36},
    {'C', 0x3e, 0x41, 0x41, 0x41, 0x22}, {'D', 0x7f, 0x41, 0x41, 0x22, 0x1c},
    {'E', 0x7f, 0x49, 0x49, 0x49, 0x41}, {'F', 0x7f, 0x09, 0x09, 0x09, 0x01},
    {'G', 0x3e, 0x41, 0x49, 0x49, 0x7a}, {'H', 0x7f, 0x08, 0x08, 0x08, 0x7f},
    {'I', 0x00, 0x41, 0x7f, 0x41, 0x00}, {'J', 0x20, 0x40, 0x41, 0x3f, 0x01},
    {'K', 0x7f, 0x08, 0x14, 0x22, 0x41}, {'L', 0x7f, 0x40, 0x40, 0x40, 0x40},
    {'M', 0x7f, 0x02, 0x0c, 0x02, 0x7f}, {'N', 0x7f, 0x04, 0x08, 0x10, 0x7f},
    {'O', 0x3e, 0x41, 0x41, 0x41, 0x3e}, {'P', 0x7f, 0x09, 0x09, 0x09, 0x06},
    {'Q', 0x3e, 0x41, 0x51, 0x21, 0x5e}, {'R', 0x7f, 0x09, 0x19, 0x29, 0x46},
    {'S', 0x46, 0x49, 0x49, 0x49, 0x31}, {'T', 0x01, 0x01, 0x7f, 0x01, 0x01},
    {'U', 0x3f, 0x40, 0x40, 0x40, 0x3f}, {'V', 0x1f, 0x20, 0x40, 0x20, 0x1f},
    {'W', 0x3f, 0x40, 0x38, 0x40, 0x3f}, {'X', 0x63, 0x14, 0x08, 0x14, 0x63},
    {'Y', 0x07, 0x08, 0x70, 0x08, 0x07}, {'Z', 0x61, 0x51, 0x49, 0x45, 0x43},
};

static const uint8_t *glyph(char c) {
    c = (char)toupper((unsigned char)c);
    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); i++) {
        if (font[i][0] == (uint8_t)c) {
            return &font[i][1];
        }
    }
    return &font[0][1];
}

static void fill(uint16_t color) {
    if (!s_fb) return;
    for (int i = 0; i < LCD_PIXELS; i++) {
        s_fb[i] = color;
    }
}

static void rect(int x, int y, int w, int h, uint16_t color) {
    for (int yy = y; yy < y + h && yy < LCD_V_RES; yy++) {
        if (yy < 0) continue;
        for (int xx = x; xx < x + w && xx < LCD_H_RES; xx++) {
            if (xx >= 0) s_fb[yy * LCD_H_RES + xx] = color;
        }
    }
}

static void text(int x, int y, const char *s, int scale, uint16_t color) {
    while (*s && x < LCD_H_RES - 6 * scale) {
        const uint8_t *g = glyph(*s++);
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (g[col] & (1 << row)) {
                    rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
        x += 6 * scale;
    }
}

static bool logo_bit(const uint8_t *bits, int index) {
    return (bits[index / 8] & (1 << (7 - (index % 8)))) != 0;
}

static void logo_mask(int x, int y, int w, int h, const uint8_t *bits,
                      uint16_t color) {
    for (int yy = 0; yy < h; yy++) {
        int dst_y = y + yy;
        if (dst_y < 0 || dst_y >= LCD_V_RES) continue;
        for (int xx = 0; xx < w; xx++) {
            int dst_x = x + xx;
            if (dst_x < 0 || dst_x >= LCD_H_RES) continue;
            if (logo_bit(bits, yy * w + xx)) {
                s_fb[dst_y * LCD_H_RES + dst_x] = color;
            }
        }
    }
}

static void draw_logo(void) {
    uint16_t black = rgb565(12, 12, 14);
    int logo_w = PS5_LOGO_W;
    int logo_h = PS5_LOGO_H;
    const uint8_t *bits = ps5_logo_bits;

    if (s_os == DISPLAY_OS_LINUX) {
        logo_w = LINUX_LOGO_W;
        logo_h = LINUX_LOGO_H;
        bits = linux_logo_bits;
    }

    int x = (LCD_H_RES - logo_w) / 2;
    int y = (106 - logo_h) / 2;
    if (y < 0) y = 0;
    logo_mask(x, y, logo_w, logo_h, bits, black);
}

esp_err_t display_init(void) {
#if !CONFIG_PSWAKE_HAS_DISPLAY
    ESP_LOGI(TAG, "display disabled by device profile");
    return ESP_OK;
#else
    ESP_RETURN_ON_ERROR(gpio_set_direction(CONFIG_PSWAKE_LCD_BL, GPIO_MODE_OUTPUT),
                        TAG, "backlight gpio");
    gpio_set_level(CONFIG_PSWAKE_LCD_BL, 1);

    spi_bus_config_t buscfg = {
        .mosi_io_num = CONFIG_PSWAKE_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = CONFIG_PSWAKE_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_PIXELS * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = CONFIG_PSWAKE_LCD_DC,
        .cs_gpio_num = CONFIG_PSWAKE_LCD_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_config, &io_handle),
                        TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = CONFIG_PSWAKE_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel),
                        TAG, "st7789");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, CONFIG_PSWAKE_LCD_SWAP_XY),
                        TAG, "swap xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, CONFIG_PSWAKE_LCD_MIRROR_X,
                                             CONFIG_PSWAKE_LCD_MIRROR_Y),
                        TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, CONFIG_PSWAKE_LCD_X_GAP,
                                              CONFIG_PSWAKE_LCD_Y_GAP),
                        TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");

    s_fb = heap_caps_malloc(LCD_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!s_fb) return ESP_ERR_NO_MEM;
    display_status("BOOT", "DISPLAY READY", "", "");
    return ESP_OK;
#endif
}

void display_set_os(display_os_t os) {
    s_os = os;
}

void display_status(const char *state, const char *line1, const char *line2,
                    const char *line3) {
#if !CONFIG_PSWAKE_HAS_DISPLAY
    ESP_LOGI(TAG, "%s %s %s %s", state ? state : "", line1 ? line1 : "",
             line2 ? line2 : "", line3 ? line3 : "");
    return;
#else
    if (!s_panel || !s_fb) return;
    uint16_t bg = rgb565(250, 250, 248);
    uint16_t band = s_os == DISPLAY_OS_LINUX ? rgb565(20, 20, 20)
                                             : rgb565(16, 80, 140);
    uint16_t fg = rgb565(230, 245, 238);
    uint16_t muted = rgb565(190, 215, 220);
    fill(bg);
    draw_logo();
    rect(0, 106, LCD_H_RES, 29, band);
    text(6, 111, state ? state : "", 1, fg);
    text(78, 111, line1 ? line1 : "", 1, fg);
    text(6, 124, line2 ? line2 : "", 1, muted);
    if (line3 && line3[0]) {
        text(168, 124, line3, 1, muted);
    }
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb);
#endif
}
