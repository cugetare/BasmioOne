/**
 * @file bsp_display.c
 * @brief sPod v2 - display and touch BSP for Guition JC4880P443C
 *
 * Display: ST7701, MIPI DSI 2-lane, 480x800 portrait
 * Touch:   GT911, I2C
 * MCU:     ESP32-P4
 */

#include "bsp_display.h"
#include "ui_app.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <assert.h>
#include <inttypes.h>

static const char *TAG = "bsp";

/* ------------------------------------------------------------------ */
/*  Board pins                                                         */
/* ------------------------------------------------------------------ */
#define BSP_LCD_RST_GPIO        GPIO_NUM_5
#define BSP_LCD_BACKLIGHT_GPIO  GPIO_NUM_23

#define BSP_TOUCH_I2C_PORT      I2C_NUM_1
#define BSP_TOUCH_SDA_GPIO      GPIO_NUM_7
#define BSP_TOUCH_SCL_GPIO      GPIO_NUM_8
#define BSP_TOUCH_INT_GPIO      GPIO_NUM_NC
#define BSP_TOUCH_RST_GPIO      GPIO_NUM_NC

/* ------------------------------------------------------------------ */
/*  Panel parameters                                                    */
/* ------------------------------------------------------------------ */
#define BSP_LCD_H_RES           480
#define BSP_LCD_V_RES           800

/* ST7701 timing used by known-working Guition examples */
#define BSP_DPI_CLK_MHZ         16
#define BSP_LCD_HSYNC           12
#define BSP_LCD_HBP             42
#define BSP_LCD_HFP             42
#define BSP_LCD_VSYNC            2
#define BSP_LCD_VBP              8
#define BSP_LCD_VFP            166

/* MIPI DSI PHY LDO */
#define BSP_MIPI_PHY_LDO_CHAN   3
#define BSP_MIPI_PHY_LDO_MV     2500

/* Backlight PWM */
#define BSP_BL_LEDC_TIMER       LEDC_TIMER_0
#define BSP_BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BSP_BL_LEDC_FREQ_HZ     24000
#define BSP_BL_LEDC_RES         LEDC_TIMER_10_BIT

/*
 * Keep the LVGL transfer buffers modest. ESP-Hosted SDIO also needs small
 * internal-DMA chunks while Wi-Fi traffic is active, and starving that pool
 * can make the hosted driver assert during uploads.
 */
#define BSP_LVGL_BUF_LINES      24


static uint32_t s_last_activity = 0;

typedef enum {
    UI_ACTIVE = 0,
    UI_DIMMED,
    UI_SCREENSAVER
} ui_idle_state_t;

static ui_idle_state_t s_idle_state = UI_ACTIVE;

/* ST7701 initialization commands from Guition sample */
static const st7701_lcd_init_cmd_t s_st7701_init_cmds[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x10, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},

    {0xB0, (uint8_t[]){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16, 0},
    {0xB1, (uint8_t[]){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},

    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x58}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4E}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t[]){0x03}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},

    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},

    {0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},

    {0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},

    {0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},

    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 20},
};

/* ------------------------------------------------------------------ */
/*  Static handles                                                      */
/* ------------------------------------------------------------------ */
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;

static void touch_i2c_scan(i2c_master_bus_handle_t i2c_bus)
{
    bool found = false;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(i2c_bus, addr, 20) == ESP_OK) {
            ESP_LOGI(TAG, "I2C device found at 0x%02X", addr);
            found = true;
        }
    }
    if (!found) {
        ESP_LOGW(TAG, "No I2C device detected on touch bus (SDA=%d, SCL=%d)",
                 BSP_TOUCH_SDA_GPIO, BSP_TOUCH_SCL_GPIO);
    }
}

/* ------------------------------------------------------------------ */
/*  LVGL callbacks                                                      */
/* ------------------------------------------------------------------ */
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_err_t err = ESP_FAIL;
    for (int tries = 0; tries < 4; tries++) {
        err = esp_lcd_panel_draw_bitmap(panel,
                                        area->x1, area->y1,
                                        area->x2 + 1, area->y2 + 1,
                                        color_map);
        if (err == ESP_OK) {
            break;
        }
        if (err == ESP_ERR_INVALID_STATE) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "draw_bitmap failed (%s)", esp_err_to_name(err));
    }
    lv_disp_flush_ready(drv);
}

static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)drv->user_data;
    esp_lcd_touch_point_data_t points[1];
    uint8_t cnt = 0;

    if (tp == NULL) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    if (esp_lcd_touch_read_data(tp) != ESP_OK) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    if (esp_lcd_touch_get_data(tp, points, &cnt, 1) != ESP_OK) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    if (cnt > 0) {
        data->point.x = (lv_coord_t)points[0].x;
        data->point.y = (lv_coord_t)points[0].y;
        data->state   = LV_INDEV_STATE_PR;
        // 🔥 ACTIVITY TRACKING + WAKEUP (delegat la UI)
        ui_wakeup();
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}


/* ------------------------------------------------------------------ */
/*  Backlight                                                           */
/* ------------------------------------------------------------------ */
static void backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BSP_BL_LEDC_RES,
        .timer_num = BSP_BL_LEDC_TIMER,
        .freq_hz = BSP_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t ch_cfg = {
        .gpio_num = BSP_LCD_BACKLIGHT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BSP_BL_LEDC_CHANNEL,
        .timer_sel = BSP_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
}

void bsp_backlight_set(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    uint32_t duty = ((uint32_t)pct * 1023u) / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BSP_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BSP_BL_LEDC_CHANNEL);
}

/* ------------------------------------------------------------------ */
/*  Public init                                                         */
/* ------------------------------------------------------------------ */
void bsp_display_init(void)
{
    /* 1) Power on MIPI DSI PHY */
    esp_ldo_channel_handle_t ldo_mipi = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = BSP_MIPI_PHY_LDO_CHAN,
        .voltage_mv = BSP_MIPI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi));
    ESP_LOGI(TAG, "MIPI DSI PHY LDO powered on");

    /* 2) MIPI DSI bus */
    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_lcd_dsi_bus_config_t bus_cfg = ST7701_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));
    ESP_LOGI(TAG, "MIPI DSI bus created");

    /* 3) DBI command IO */
    esp_lcd_panel_io_handle_t dbi_io = NULL;
    esp_lcd_dbi_io_config_t dbi_cfg = ST7701_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io));

    /* 4) ST7701 panel */
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = BSP_DPI_CLK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = BSP_LCD_H_RES,
            .v_size = BSP_LCD_V_RES,
            .hsync_pulse_width = BSP_LCD_HSYNC,
            .hsync_back_porch = BSP_LCD_HBP,
            .hsync_front_porch = BSP_LCD_HFP,
            .vsync_pulse_width = BSP_LCD_VSYNC,
            .vsync_back_porch = BSP_LCD_VBP,
            .vsync_front_porch = BSP_LCD_VFP,
        },
        .flags.use_dma2d = true,
    };

    st7701_vendor_config_t vendor_cfg = {
        .init_cmds = s_st7701_init_cmds,
        .init_cmds_size = sizeof(s_st7701_init_cmds) / sizeof(s_st7701_init_cmds[0]),
        .mipi_config = {
            .dsi_bus = dsi_bus,
            .dpi_config = &dpi_cfg,
        },
        .flags = {
            .use_mipi_interface = 1,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(dbi_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    ESP_LOGI(TAG, "ST7701 panel ready (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);

    /* 5) Backlight */
    backlight_init();
    bsp_backlight_set(100);

    /* 6) Touch (non-fatal if not present) */
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = BSP_TOUCH_I2C_PORT,
        .sda_io_num = BSP_TOUCH_SDA_GPIO,
        .scl_io_num = BSP_TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    bool touch_ready = false;
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 16,
        .lcd_param_bits = 8,
        .flags.disable_control_phase = true,
    };

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_TOUCH_RST_GPIO,
        .int_gpio_num = BSP_TOUCH_INT_GPIO,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };

    const uint16_t gt911_addrs[] = {
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
        ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP,
    };

    for (size_t i = 0; i < sizeof(gt911_addrs) / sizeof(gt911_addrs[0]); i++) {
        tp_io_cfg.dev_addr = gt911_addrs[i];
        esp_err_t err = esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GT911 IO create failed at address 0x%02" PRIX32 " (%s)",
                     tp_io_cfg.dev_addr, esp_err_to_name(err));
            continue;
        }

        err = esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "GT911 touch ready at I2C address 0x%02" PRIX32, tp_io_cfg.dev_addr);
            touch_ready = true;
            break;
        }

        ESP_LOGW(TAG, "GT911 init failed at address 0x%02" PRIX32 " (%s); trying fallback",
                 tp_io_cfg.dev_addr, esp_err_to_name(err));
        esp_lcd_panel_io_del(tp_io);
        tp_io = NULL;
    }

    if (!touch_ready) {
        ESP_LOGW(TAG, "GT911 not detected; continuing without touch");
        touch_i2c_scan(i2c_bus);
    }

    /* 7) LVGL */
    lv_init();

    ESP_LOGI(TAG, "LVGL cfg: color_depth=%d screen_transp=%d refr_ms=%d",
             (int)LV_COLOR_DEPTH, (int)LV_COLOR_SCREEN_TRANSP, (int)LV_DISP_DEF_REFR_PERIOD);

    size_t buf_px = (size_t)BSP_LCD_H_RES * BSP_LVGL_BUF_LINES;
    size_t buf_bytes = buf_px * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    //lv_color_t *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM  | MALLOC_CAP_DMA);
    //lv_color_t *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM  | MALLOC_CAP_DMA);
    ESP_LOGI("LVGL", "buf1=%p buf2=%p size=%d", buf1, buf2, buf_bytes);
    assert(buf1 && buf2 && "Internal RAM allocation failed for LVGL draw buffers");
    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, (lv_coord_t)buf_px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = BSP_LCD_H_RES;
    s_disp_drv.ver_res = BSP_LCD_V_RES;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.user_data = s_panel;
    lv_disp_drv_register(&s_disp_drv);

    if (touch_ready) {
        lv_indev_drv_init(&s_indev_drv);
        s_indev_drv.type = LV_INDEV_TYPE_POINTER;
        s_indev_drv.read_cb = lvgl_touch_cb;
        s_indev_drv.user_data = s_touch;
        lv_indev_drv_register(&s_indev_drv);
    }

    ESP_LOGI(TAG, "LVGL ready");
}
