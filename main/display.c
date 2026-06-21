#include "display.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "logo_assets.h"

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 135
#define LCD_V_RES 240
#define LCD_PIXELS (LCD_H_RES * LCD_V_RES)
#define LCD_LOGO_AREA_H ((LCD_V_RES * 2) / 3)
#define LCD_TEXT_AREA_Y LCD_LOGO_AREA_H
#define LCD_TEXT_AREA_H (LCD_V_RES - LCD_TEXT_AREA_Y)
#define LCD_BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define LCD_BL_LEDC_TIMER LEDC_TIMER_0
#define LCD_BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define LCD_BL_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define LCD_BL_LEDC_FREQ_HZ 5000
#define LCD_BL_LEDC_DUTY_MAX ((1 << 10) - 1)

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

#ifndef CONFIG_PSWAKE_LCD_BRIGHTNESS
#define CONFIG_PSWAKE_LCD_BRIGHTNESS 70
#endif

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static uint16_t *s_bottom_row;
static display_os_t s_os = DISPLAY_OS_PS5;
static char s_net_status[24];

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}

static uint16_t blend565(uint16_t dst, uint16_t src, uint8_t alpha) {
    if (alpha == 0) return dst;
    if (alpha == 255) return src;
    int dr = ((dst >> 11) & 0x1f) * 255 / 31;
    int dg = ((dst >> 5) & 0x3f) * 255 / 63;
    int db = (dst & 0x1f) * 255 / 31;
    int sr = ((src >> 11) & 0x1f) * 255 / 31;
    int sg = ((src >> 5) & 0x3f) * 255 / 63;
    int sb = (src & 0x1f) * 255 / 31;
    uint8_t r = (uint8_t)((sr * alpha + dr * (255 - alpha)) / 255);
    uint8_t g = (uint8_t)((sg * alpha + dg * (255 - alpha)) / 255);
    uint8_t b = (uint8_t)((sb * alpha + db * (255 - alpha)) / 255);
    return rgb565(r, g, b);
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

static int text_width(const char *s, int scale) {
    return s ? (int)strlen(s) * 6 * scale : 0;
}

static void text_centered(int x, int y, int w, const char *s, int scale,
                          uint16_t color) {
    int tw = text_width(s, scale);
    int tx = x + (w - tw) / 2;
    if (tx < x) tx = x;
    text(tx, y, s ? s : "", scale, color);
}

static bool logo_bit(const uint8_t *bits, int index) {
    return (bits[index / 8] & (1 << (7 - (index % 8)))) != 0;
}

static void logo_mask_scaled(int x, int y, int src_w, int src_h, int dst_w,
                             int dst_h, const uint8_t *bits, uint16_t color) {
    for (int yy = 0; yy < dst_h; yy++) {
        int dst_y = y + yy;
        if (dst_y < 0 || dst_y >= LCD_V_RES) continue;
        int src_y = (yy * src_h) / dst_h;
        for (int xx = 0; xx < dst_w; xx++) {
            int dst_x = x + xx;
            if (dst_x < 0 || dst_x >= LCD_H_RES) continue;
            int src_x = (xx * src_w) / dst_w;
            if (logo_bit(bits, src_y * src_w + src_x)) {
                s_fb[dst_y * LCD_H_RES + dst_x] = color;
            }
        }
    }
}

static void logo_alpha(int x, int y, int w, int h, const uint8_t *alpha,
                       uint16_t color) {
    for (int yy = 0; yy < h; yy++) {
        int dst_y = y + yy;
        if (dst_y < 0 || dst_y >= LCD_V_RES) continue;
        for (int xx = 0; xx < w; xx++) {
            int dst_x = x + xx;
            if (dst_x < 0 || dst_x >= LCD_H_RES) continue;
            uint8_t a = alpha[yy * w + xx];
            uint16_t *dst = &s_fb[dst_y * LCD_H_RES + dst_x];
            *dst = blend565(*dst, color, a);
        }
    }
}

static void draw_logo(uint16_t color) {
#if LINUX_LOGO_ALPHA
    if (s_os == DISPLAY_OS_LINUX) {
        int x = (LCD_H_RES - LINUX_LOGO_W) / 2;
        int y = (LCD_LOGO_AREA_H - LINUX_LOGO_H) / 2;
        if (y < 0) y = 0;
        logo_alpha(x, y, LINUX_LOGO_W, LINUX_LOGO_H, linux_logo_alpha, color);
        return;
    }
#endif

    int logo_w = PS5_LOGO_W;
    int logo_h = PS5_LOGO_H;
    const uint8_t *bits = ps5_logo_bits;

    const int max_w = LCD_H_RES - 10;
    const int max_h = LCD_LOGO_AREA_H - 10;
    int draw_w = max_w;
    int draw_h = (logo_h * draw_w) / logo_w;
    if (draw_h > max_h) {
        draw_h = max_h;
        draw_w = (logo_w * draw_h) / logo_h;
    }

    int x = (LCD_H_RES - draw_w) / 2;
    int y = (LCD_LOGO_AREA_H - draw_h) / 2;
    if (y < 0) y = 0;
    logo_mask_scaled(x, y, logo_w, logo_h, draw_w, draw_h, bits, color);
}

static esp_err_t backlight_init(void) {
    int brightness = CONFIG_PSWAKE_LCD_BRIGHTNESS;
    if (brightness < 0) brightness = 0;
    if (brightness > 100) brightness = 100;

    ledc_timer_config_t timer_config = {
        .speed_mode = LCD_BL_LEDC_MODE,
        .timer_num = LCD_BL_LEDC_TIMER,
        .duty_resolution = LCD_BL_LEDC_DUTY_RES,
        .freq_hz = LCD_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG, "backlight timer");

    ledc_channel_config_t channel_config = {
        .gpio_num = CONFIG_PSWAKE_LCD_BL,
        .speed_mode = LCD_BL_LEDC_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .duty = (LCD_BL_LEDC_DUTY_MAX * brightness) / 100,
        .hpoint = 0,
    };
    return ledc_channel_config(&channel_config);
}

esp_err_t display_init(void) {
#if !CONFIG_PSWAKE_HAS_DISPLAY
    ESP_LOGI(TAG, "display disabled by device profile");
    return ESP_OK;
#else
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

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
    s_bottom_row = heap_caps_malloc(LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!s_bottom_row) return ESP_ERR_NO_MEM;
    display_status("BOOT", "DISPLAY READY", "", "");
    return ESP_OK;
#endif
}

void display_set_os(display_os_t os) {
    s_os = os;
}

void display_set_net_status(const char *status) {
#if !CONFIG_PSWAKE_HAS_DISPLAY
    ESP_LOGI(TAG, "net=%s", status ? status : "");
#else
    strlcpy(s_net_status, status ? status : "", sizeof(s_net_status));
#endif
}

void display_status(const char *state, const char *line1, const char *line2,
                    const char *line3) {
#if !CONFIG_PSWAKE_HAS_DISPLAY
    ESP_LOGI(TAG, "%s %s %s %s", state ? state : "", line1 ? line1 : "",
             line2 ? line2 : "", line3 ? line3 : "");
    return;
#else
    if (!s_panel || !s_fb) return;
    bool linux_theme = s_os == DISPLAY_OS_LINUX;
    uint16_t bg = rgb565(250, 250, 248);
    uint16_t logo = rgb565(12, 12, 14);
    uint16_t band = linux_theme ? rgb565(0, 0, 0) : rgb565(16, 80, 140);
    uint16_t fg = rgb565(230, 245, 238);
    uint16_t muted = rgb565(190, 215, 220);
    const int band_y = LCD_TEXT_AREA_Y;
    const int band_h = LCD_TEXT_AREA_H;
    const char *primary = (line1 && line1[0]) ? line1 : state;
    const char *secondary = (line2 && line2[0]) ? line2 : line3;
    bool show_secondary = secondary && secondary[0] &&
                          text_width(secondary, 1) <= LCD_H_RES - 8;
    bool show_net = s_net_status[0] &&
                    text_width(s_net_status, 1) <= LCD_H_RES - 8;
    int primary_scale = text_width(primary, 3) <= (LCD_H_RES - 8) ? 3 :
                        text_width(primary, 2) <= (LCD_H_RES - 8) ? 2 : 1;
    char primary_top[24] = {0};
    char primary_bottom[24] = {0};
    bool primary_wrapped = false;
    const char *split = strchr(primary, ' ');
    if (primary_scale == 1 && split) {
        size_t top_len = split - primary;
        if (top_len > 0 && top_len < sizeof(primary_top) &&
            strlen(split + 1) < sizeof(primary_bottom)) {
            memcpy(primary_top, primary, top_len);
            primary_top[top_len] = 0;
            strlcpy(primary_bottom, split + 1, sizeof(primary_bottom));
            primary_wrapped = text_width(primary_top, 2) <= (LCD_H_RES - 8) &&
                              text_width(primary_bottom, 2) <= (LCD_H_RES - 8);
            if (primary_wrapped) primary_scale = 2;
        }
    }
    int footer_lines = (show_secondary ? 1 : 0) + (show_net ? 1 : 0);
    int footer_y = footer_lines ? band_y + band_h - footer_lines * 9 - 2 : 0;
    int primary_area_h = footer_lines ? footer_y - band_y - 2 : band_h;
    int primary_h = primary_wrapped ? 14 * primary_scale + 4 : 7 * primary_scale;
    int primary_y = band_y + (primary_area_h - primary_h) / 2;
    if (primary_y < band_y + 4) primary_y = band_y + 4;

    fill(bg);
    draw_logo(logo);
    rect(0, band_y, LCD_H_RES, band_h, band);
    if (primary_wrapped) {
        text_centered(0, primary_y, LCD_H_RES, primary_top, primary_scale, fg);
        text_centered(0, primary_y + 7 * primary_scale + 4, LCD_H_RES,
                      primary_bottom, primary_scale, fg);
    } else {
        text_centered(0, primary_y, LCD_H_RES, primary, primary_scale, fg);
    }
    if (show_secondary) {
        text_centered(0, footer_y, LCD_H_RES, secondary, 1, muted);
        footer_y += 9;
    }
    if (show_net) {
        text_centered(0, footer_y, LCD_H_RES, s_net_status, 1, muted);
    }
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_H_RES, LCD_V_RES, s_fb);
    if (s_bottom_row) {
        for (int x = 0; x < LCD_H_RES; x++) {
            s_bottom_row[x] = band;
        }
        esp_lcd_panel_draw_bitmap(s_panel, 0, LCD_V_RES - 1, LCD_H_RES,
                                  LCD_V_RES, s_bottom_row);
        esp_lcd_panel_draw_bitmap(s_panel, 0, LCD_V_RES, LCD_H_RES,
                                  LCD_V_RES + 1, s_bottom_row);
    }
#endif
}
