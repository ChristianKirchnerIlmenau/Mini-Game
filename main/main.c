#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

#define TAG "pong"

// LCD pins (as provided)
#define LCD_MOSI 23
#define LCD_SCLK 18
#define LCD_CS   15
#define LCD_DC    2
#define LCD_RST   CONFIG_PONG_LCD_RST_GPIO
#define LCD_BLK  32

#define LCD_HOST SPI2_HOST

#define SCREEN_W CONFIG_PONG_SCREEN_WIDTH
#define SCREEN_H CONFIG_PONG_SCREEN_HEIGHT

#define PADDLE_H 4
#define PADDLE_W (SCREEN_W / 5)
#define BALL_SIZE CONFIG_PONG_BALL_SIZE

#define BALL_BASE_SPEED 1
#define BALL_SPEED_STEP_HITS 5
#define BALL_MAX_SPEED 6
#define FAIL_LIMIT 10

#define GPIO_LEFT  CONFIG_PONG_GPIO_LEFT
#define GPIO_RIGHT CONFIG_PONG_GPIO_RIGHT
#define GPIO_PAUSE CONFIG_PONG_GPIO_PAUSE

#define LCD_OFFSET_X CONFIG_PONG_LCD_OFFSET_X
#define LCD_OFFSET_Y CONFIG_PONG_LCD_OFFSET_Y

#ifdef CONFIG_PONG_LCD_SWAP_XY
#define LCD_SWAP_XY true
#else
#define LCD_SWAP_XY false
#endif

#ifdef CONFIG_PONG_LCD_MIRROR_X
#define LCD_MIRROR_X true
#else
#define LCD_MIRROR_X false
#endif

#ifdef CONFIG_PONG_LCD_MIRROR_Y
#define LCD_MIRROR_Y true
#else
#define LCD_MIRROR_Y false
#endif

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF

typedef struct {
    int x;
    int y;
    int vx;
    int vy;
} ball_t;

typedef struct {
    int x;
} paddle_t;

typedef struct {
    int gpio;
    int stable_level;
    int last_level;
    int stable_count;
    TickType_t pressed_since;
} button_t;

typedef enum {
    STATE_START,
    STATE_RUN,
    STATE_PAUSE
} game_state_t;

static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_framebuffer = NULL;

static void display_clear(uint16_t color);
static void display_flush(void);

static void display_init(void)
{
    ESP_LOGI(TAG, "Display init (ST7789)");

    gpio_config_t bk_conf = {
        .pin_bit_mask = 1ULL << LCD_BLK,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&bk_conf);
    gpio_set_level(LCD_BLK, 1);

    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SCREEN_W * SCREEN_H * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DC,
        .cs_gpio_num = LCD_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, LCD_OFFSET_X, LCD_OFFSET_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    size_t fb_size = SCREEN_W * SCREEN_H * sizeof(uint16_t);
    s_framebuffer = heap_caps_malloc(fb_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_framebuffer) {
        ESP_LOGE(TAG, "Framebuffer allocation failed");
        return;
    }

    display_clear(COLOR_BLACK);
    display_flush();
}

static void display_clear(uint16_t color)
{
    if (!s_framebuffer) {
        return;
    }
    for (int i = 0; i < SCREEN_W * SCREEN_H; ++i) {
        s_framebuffer[i] = color;
    }
}

static void display_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_framebuffer) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > SCREEN_W) {
        w = SCREEN_W - x;
    }
    if (y + h > SCREEN_H) {
        h = SCREEN_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }

    for (int yy = y; yy < y + h; ++yy) {
        uint16_t *row = s_framebuffer + yy * SCREEN_W + x;
        for (int xx = 0; xx < w; ++xx) {
            row[xx] = color;
        }
    }
}

static void display_flush(void)
{
    if (!s_panel || !s_framebuffer) {
        return;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, SCREEN_W, SCREEN_H, s_framebuffer));
}

static void buttons_init(void)
{
    uint64_t mask = 0;
#if CONFIG_PONG_GPIO_LEFT >= 0
    mask |= (1ULL << GPIO_LEFT);
#endif
#if CONFIG_PONG_GPIO_RIGHT >= 0
    mask |= (1ULL << GPIO_RIGHT);
#endif
#if CONFIG_PONG_GPIO_PAUSE >= 0
    mask |= (1ULL << GPIO_PAUSE);
#endif
    if (mask == 0) {
        return;
    }

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = mask,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };

    gpio_config(&io_conf);
}

static int clamp(int value, int min, int max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

// Public-domain 8x8 ASCII font (font8x8_basic)
static const uint8_t font8x8_basic[128][8] = {
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    { 0x7E,0x81,0xA5,0x81,0xBD,0x99,0x81,0x7E },
    { 0x7E,0xFF,0xDB,0xFF,0xC3,0xE7,0xFF,0x7E },
    { 0x6C,0xFE,0xFE,0xFE,0x7C,0x38,0x10,0x00 },
    { 0x10,0x38,0x7C,0xFE,0x7C,0x38,0x10,0x00 },
    { 0x38,0x7C,0x38,0xFE,0xFE,0xD6,0x10,0x38 },
    { 0x10,0x38,0x7C,0xFE,0xFE,0x7C,0x10,0x38 },
    { 0x00,0x00,0x18,0x3C,0x3C,0x18,0x00,0x00 },
    { 0xFF,0xFF,0xE7,0xC3,0xC3,0xE7,0xFF,0xFF },
    { 0x00,0x3C,0x66,0x42,0x42,0x66,0x3C,0x00 },
    { 0xFF,0xC3,0x99,0xBD,0xBD,0x99,0xC3,0xFF },
    { 0x0F,0x07,0x0F,0x7D,0xCC,0xCC,0xCC,0x78 },
    { 0x3C,0x66,0x66,0x66,0x3C,0x18,0x7E,0x18 },
    { 0x3F,0x33,0x3F,0x30,0x30,0x70,0xF0,0xE0 },
    { 0x7F,0x63,0x7F,0x63,0x63,0x67,0xE6,0xC0 },
    { 0x99,0x5A,0x3C,0xE7,0xE7,0x3C,0x5A,0x99 },
    { 0x80,0xE0,0xF8,0xFE,0xF8,0xE0,0x80,0x00 },
    { 0x02,0x0E,0x3E,0xFE,0x3E,0x0E,0x02,0x00 },
    { 0x18,0x3C,0x7E,0x18,0x18,0x7E,0x3C,0x18 },
    { 0x66,0x66,0x66,0x66,0x66,0x00,0x66,0x00 },
    { 0x7F,0xDB,0xDB,0x7B,0x1B,0x1B,0x1B,0x00 },
    { 0x3E,0x63,0x38,0x6C,0x6C,0x38,0xCC,0x78 },
    { 0x00,0x00,0x00,0x00,0x7E,0x7E,0x7E,0x00 },
    { 0x18,0x3C,0x7E,0x18,0x7E,0x3C,0x18,0xFF },
    { 0x18,0x3C,0x7E,0x18,0x18,0x18,0x18,0x00 },
    { 0x18,0x18,0x18,0x18,0x7E,0x3C,0x18,0x00 },
    { 0x00,0x18,0x0C,0xFE,0x0C,0x18,0x00,0x00 },
    { 0x00,0x30,0x60,0xFE,0x60,0x30,0x00,0x00 },
    { 0x00,0x00,0xC0,0xC0,0xC0,0xFE,0x00,0x00 },
    { 0x00,0x24,0x66,0xFF,0x66,0x24,0x00,0x00 },
    { 0x00,0x18,0x3C,0x7E,0xFF,0xFF,0x00,0x00 },
    { 0x00,0xFF,0xFF,0x7E,0x3C,0x18,0x00,0x00 },
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    { 0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00 },
    { 0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00 },
    { 0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00 },
    { 0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00 },
    { 0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00 },
    { 0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00 },
    { 0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00 },
    { 0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00 },
    { 0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00 },
    { 0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00 },
    { 0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00 },
    { 0x00,0x00,0x00,0x00,0x18,0x18,0x30,0x00 },
    { 0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00 },
    { 0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00 },
    { 0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00 },
    { 0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00 },
    { 0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00 },
    { 0x7C,0xC6,0x0E,0x1C,0x70,0xC0,0xFE,0x00 },
    { 0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00 },
    { 0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00 },
    { 0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00 },
    { 0x3C,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00 },
    { 0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00 },
    { 0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00 },
    { 0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00 },
    { 0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00 },
    { 0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00 },
    { 0x0E,0x1C,0x38,0x70,0x38,0x1C,0x0E,0x00 },
    { 0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00 },
    { 0x70,0x38,0x1C,0x0E,0x1C,0x38,0x70,0x00 },
    { 0x7C,0xC6,0x0E,0x1C,0x18,0x00,0x18,0x00 },
    { 0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x7C,0x00 },
    { 0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00 },
    { 0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00 },
    { 0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00 },
    { 0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00 },
    { 0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00 },
    { 0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00 },
    { 0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00 },
    { 0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00 },
    { 0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00 },
    { 0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00 },
    { 0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00 },
    { 0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00 },
    { 0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00 },
    { 0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00 },
    { 0x38,0x6C,0xC6,0xC6,0xC6,0x6C,0x38,0x00 },
    { 0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00 },
    { 0x38,0x6C,0xC6,0xC6,0xDA,0xCC,0x76,0x00 },
    { 0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00 },
    { 0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00 },
    { 0x7E,0x7E,0x5A,0x18,0x18,0x18,0x3C,0x00 },
    { 0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00 },
    { 0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00 },
    { 0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00 },
    { 0xC6,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00 },
    { 0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00 },
    { 0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00 },
    { 0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00 },
    { 0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00 },
    { 0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00 },
    { 0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00 },
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF },
    { 0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00 },
    { 0x00,0x00,0x7C,0x06,0x7E,0xC6,0x7E,0x00 },
    { 0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00 },
    { 0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00 },
    { 0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00 },
    { 0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00 },
    { 0x3C,0x66,0x60,0xF8,0x60,0x60,0xF0,0x00 },
    { 0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8 },
    { 0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00 },
    { 0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00 },
    { 0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C },
    { 0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00 },
    { 0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00 },
    { 0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xC6,0x00 },
    { 0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00 },
    { 0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00 },
    { 0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0 },
    { 0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E },
    { 0x00,0x00,0xDC,0x76,0x66,0x60,0xF0,0x00 },
    { 0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00 },
    { 0x30,0x30,0xFC,0x30,0x30,0x36,0x1C,0x00 },
    { 0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00 },
    { 0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00 },
    { 0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00 },
    { 0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00 },
    { 0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0xFC },
    { 0x00,0x00,0xFE,0x4C,0x18,0x32,0xFE,0x00 },
    { 0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00 },
    { 0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00 },
    { 0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00 },
    { 0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00 },
    { 0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0x00 }
};

static void draw_char(int x, int y, char c, int scale)
{
    uint8_t index = (uint8_t)c;
    if (index > 127) {
        index = (uint8_t)'?';
    }
    for (int row = 0; row < 8; ++row) {
        uint8_t bits = font8x8_basic[index][row];
        for (int col = 0; col < 8; ++col) {
            if (bits & (1 << (7 - col))) {
                display_draw_rect(x + col * scale, y + row * scale, scale, scale, COLOR_WHITE);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, int scale)
{
    int cursor = x;
    for (const char *p = text; *p; ++p) {
        draw_char(cursor, y, *p, scale);
        cursor += (8 * scale) + scale;
    }
}

static bool button_update(button_t *btn, TickType_t now, int debounce_cycles)
{
    if (btn->gpio < 0) {
        return false;
    }
    int level = gpio_get_level(btn->gpio);
    if (level != btn->last_level) {
        btn->last_level = level;
        btn->stable_count = 0;
    } else if (btn->stable_count < debounce_cycles) {
        btn->stable_count++;
    }

    if (btn->stable_count == debounce_cycles && level != btn->stable_level) {
        btn->stable_level = level;
        if (level == 0) {
            btn->pressed_since = now;
            return true;
        }
    }
    return false;
}

static int nvs_load_highscore(void)
{
    nvs_handle_t handle;
    int highscore = 0;
    if (nvs_open("pong", NVS_READONLY, &handle) == ESP_OK) {
        int32_t value = 0;
        if (nvs_get_i32(handle, "highscore", &value) == ESP_OK) {
            highscore = (int)value;
        }
        nvs_close(handle);
    }
    return highscore;
}

static void nvs_save_highscore(int highscore)
{
    nvs_handle_t handle;
    if (nvs_open("pong", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_i32(handle, "highscore", highscore);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void game_reset(ball_t *ball, paddle_t *paddle, int *hits, int *misses)
{
    paddle->x = SCREEN_W / 2 - PADDLE_W / 2;
    ball->x = SCREEN_W / 2;
    ball->y = SCREEN_H / 2;
    ball->vx = BALL_BASE_SPEED;
    ball->vy = BALL_BASE_SPEED;
    *hits = 0;
    *misses = 0;
}

static int ball_speed_for_hits(int hits)
{
    int speed = BALL_BASE_SPEED + hits / BALL_SPEED_STEP_HITS;
    if (speed > BALL_MAX_SPEED) {
        speed = BALL_MAX_SPEED;
    }
    return speed;
}

static void game_step(ball_t *ball, paddle_t *paddle, int *hits, int *misses)
{
    ball->x += ball->vx;
    ball->y += ball->vy;

    if (ball->x <= 0 || ball->x + BALL_SIZE >= SCREEN_W) {
        ball->vx = -ball->vx;
    }

    if (ball->y <= 0) {
        ball->vy = -ball->vy;
    }

    int paddle_y = SCREEN_H - PADDLE_H - 2;
    if (ball->y + BALL_SIZE >= paddle_y) {
        if (ball->x + BALL_SIZE >= paddle->x && ball->x <= paddle->x + PADDLE_W) {
            ball->vy = -ball->vy;
            ball->y = paddle_y - BALL_SIZE - 1;
            (*hits)++;
            int speed = ball_speed_for_hits(*hits);
            ball->vx = (ball->vx < 0) ? -speed : speed;
            ball->vy = -speed;
        } else if (ball->y + BALL_SIZE >= SCREEN_H) {
            (*misses)++;
            ball->x = SCREEN_W / 2;
            ball->y = SCREEN_H / 2;
            int speed = ball_speed_for_hits(0);
            ball->vx = (ball->vx > 0) ? -speed : speed;
            ball->vy = speed;
        }
    }
}

static void game_render(const ball_t *ball, const paddle_t *paddle, int hits, int misses, bool show_highscore, int highscore, bool paused)
{
    display_clear(COLOR_BLACK);

    int paddle_y = SCREEN_H - PADDLE_H - 2;
    display_draw_rect(paddle->x, paddle_y, PADDLE_W, PADDLE_H, COLOR_WHITE);
    display_draw_rect(ball->x, ball->y, BALL_SIZE, BALL_SIZE, COLOR_WHITE);

    char buf[32];
    if (show_highscore) {
        snprintf(buf, sizeof(buf), "HISCORE:%d H:%d F:%d", highscore, hits, misses);
    } else {
        snprintf(buf, sizeof(buf), "H:%d F:%d", hits, misses);
    }
    int hud_scale = show_highscore ? 1 : 2;
    draw_text(2, 2, buf, hud_scale);

    if (paused) {
        draw_text((SCREEN_W / 2) - 20, (SCREEN_H / 2) - 4, "PAUSE", 1);
    }

    display_flush();
}

static void render_start_screen(int highscore)
{
    display_clear(COLOR_BLACK);

    const char *title = "PONG";
    int title_scale = 3;
    int title_w = (int)strlen(title) * ((8 * title_scale) + title_scale);
    int title_x = (SCREEN_W - title_w) / 2;
    draw_text(title_x, 20, title, title_scale);

    char buf[32];
    snprintf(buf, sizeof(buf), "HIGH:%d", highscore);
    int info_scale = 2;
    int info_w = (int)strlen(buf) * ((8 * info_scale) + info_scale);
    int info_x = (SCREEN_W - info_w) / 2;
    draw_text(info_x, 70, buf, info_scale);

    draw_text(20, SCREEN_H - 20, "PRESS BOOT", 1);
    display_flush();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Pong start");

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed, highscore will not persist");
    }

    display_init();
    buttons_init();

    paddle_t paddle = { .x = SCREEN_W / 2 - PADDLE_W / 2 };

    ball_t ball = {
        .x = SCREEN_W / 2,
        .y = SCREEN_H / 2,
        .vx = BALL_BASE_SPEED,
        .vy = BALL_BASE_SPEED
    };

    const int paddle_speed = 3;
    const TickType_t frame_delay = pdMS_TO_TICKS(16);
    const int debounce_cycles = 3;
    const TickType_t long_press_ms = pdMS_TO_TICKS(800);

    button_t left_btn = { .gpio = GPIO_LEFT, .stable_level = 1, .last_level = 1, .stable_count = 0, .pressed_since = 0 };
    button_t right_btn = { .gpio = GPIO_RIGHT, .stable_level = 1, .last_level = 1, .stable_count = 0, .pressed_since = 0 };
    button_t pause_btn = { .gpio = GPIO_PAUSE, .stable_level = 1, .last_level = 1, .stable_count = 0, .pressed_since = 0 };

    int hits = 0;
    int misses = 0;
    int highscore = nvs_load_highscore();
    bool show_highscore = false;
    game_state_t state = STATE_START;

    if (GPIO_PAUSE == 0) {
        ESP_LOGW(TAG, "Pause on GPIO0 (BOOT). Do not hold during reset.");
    }

    while (true) {
        TickType_t now = xTaskGetTickCount();

        button_update(&left_btn, now, debounce_cycles);
        button_update(&right_btn, now, debounce_cycles);
        if (button_update(&pause_btn, now, debounce_cycles)) {
            if (state == STATE_START) {
                game_reset(&ball, &paddle, &hits, &misses);
                state = STATE_RUN;
            } else if (state == STATE_RUN) {
                state = STATE_PAUSE;
            } else {
                state = STATE_RUN;
            }
        }

        bool left_pressed = (left_btn.gpio >= 0) && (left_btn.stable_level == 0);
        bool right_pressed = (right_btn.gpio >= 0) && (right_btn.stable_level == 0);

        if (left_pressed) {
            paddle.x -= paddle_speed;
        }
        if (right_pressed) {
            paddle.x += paddle_speed;
        }
        paddle.x = clamp(paddle.x, 0, SCREEN_W - PADDLE_W);

        if (state == STATE_START) {
            render_start_screen(highscore);
            vTaskDelay(frame_delay);
            continue;
        }

        show_highscore = false;
        if (left_pressed && (now - left_btn.pressed_since) >= long_press_ms) {
            show_highscore = true;
        }
        if (right_pressed && (now - right_btn.pressed_since) >= long_press_ms) {
            show_highscore = true;
        }

        if (state == STATE_RUN) {
            game_step(&ball, &paddle, &hits, &misses);
            if (hits > highscore) {
                highscore = hits;
                nvs_save_highscore(highscore);
            }
            if (misses >= FAIL_LIMIT) {
                state = STATE_START;
                game_reset(&ball, &paddle, &hits, &misses);
                vTaskDelay(frame_delay);
                continue;
            }
        }

        game_render(&ball, &paddle, hits, misses, show_highscore, highscore, state == STATE_PAUSE);

        vTaskDelay(frame_delay);
    }
}
