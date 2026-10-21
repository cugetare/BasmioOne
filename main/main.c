#include "sdkconfig.h"
#include "bsp_display.h"
#if defined(CONFIG_SPOD_C6_DEBUG_BRIDGE) && CONFIG_SPOD_C6_DEBUG_BRIDGE
#include "c6_debug_bridge.h"
#endif
#include "ui_app.h"
#include "ui_screens.h"
#include "audio_backend.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

#define NORMAL_BRIGHTNESS       100
#define SAVER_VU_TIMER_MS       120

#define HW_SLEEP_BTN_GPIO       GPIO_NUM_33
#define HW_POWER_OFF_GPIO       GPIO_NUM_31
#define HW_BTN_POLL_MS          20
#define HW_BTN_DEBOUNCE_MS      35
#define HW_BTN_LONG_PRESS_MS    1500
#define HW_POWER_OFF_PULSE_MS   90

static int s_bl_current = 100;
static int s_bl_target  = 100;
static uint8_t s_user_brightness = NORMAL_BRIGHTNESS;
static uint8_t s_min_brightness = 20;
static uint32_t s_dim_after_ms = 20u * 1000u;
static uint32_t s_saver_after_ms = 30u * 1000u;
static uint32_t s_idle_off_ms = 10u * 60u * 1000u;
static void screensaver_create(void);
static void screensaver_destroy(void);
static void idle_check_cb(lv_timer_t *t);
static void hw_buttons_poll_cb(lv_timer_t *t);

typedef enum {
    UI_ACTIVE = 0,
    UI_DIMMED,
    UI_SCREENSAVER
} ui_idle_state_t;

static ui_idle_state_t s_idle_state = UI_ACTIVE;
static uint32_t s_last_activity = 0;
static lv_obj_t *s_screensaver = NULL;
static lv_obj_t *s_vu_wrap = NULL;
static lv_obj_t *s_vu_bars[22] = {0};
static lv_timer_t *s_saver_vu_timer = NULL;
static volatile bool s_boot_tone_done = false;
static volatile esp_err_t s_boot_tone_ret = ESP_FAIL;
static const char *TAG = "app_main";
static volatile bool s_shutdown_task_started = false;

extern const uint8_t boot_logo_ro_start[] asm("_binary_boot_ro_basmio_png_start");
extern const uint8_t boot_logo_ro_end[] asm("_binary_boot_ro_basmio_png_end");
extern const uint8_t boot_logo_en_start[] asm("_binary_boot_en_basmio_png_start");
extern const uint8_t boot_logo_en_end[] asm("_binary_boot_en_basmio_png_end");

static bool s_hw_sleep_raw_prev = false;
static bool s_hw_sleep_stable = false;
static uint32_t s_hw_sleep_last_change_ms = 0;
static uint32_t s_hw_sleep_press_start_ms = 0;
static bool s_hw_sleep_long_handled = false;

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(1);
}

static void boot_dot_opa_cb(void *obj, int32_t opa)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)opa, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void boot_pump_ui_ms(uint32_t ms)
{
    uint32_t start = lv_tick_get();
    while (lv_tick_elaps(start) < ms) {
        uint32_t d = lv_timer_handler();
        if (d == 0 || d > 10) {
            d = 10;
        }
        vTaskDelay(pdMS_TO_TICKS(d));
    }
}

static void boot_show_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    const spod_app_state_t *state = ui_app_get_state();
    const bool ro = (state && state->language != 0);
    static lv_img_dsc_t logo_ro;
    static lv_img_dsc_t logo_en;
    if (!logo_ro.data) {
        logo_ro.header.always_zero = 0;
        logo_ro.header.w = 0;
        logo_ro.header.h = 0;
        logo_ro.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        logo_ro.data = boot_logo_ro_start;
        logo_ro.data_size = (uint32_t)(boot_logo_ro_end - boot_logo_ro_start);

        logo_en.header.always_zero = 0;
        logo_en.header.w = 0;
        logo_en.header.h = 0;
        logo_en.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        logo_en.data = boot_logo_en_start;
        logo_en.data_size = (uint32_t)(boot_logo_en_end - boot_logo_en_start);
    }

    lv_obj_t *logo = lv_img_create(scr);
    lv_img_set_src(logo, ro ? &logo_ro : &logo_en);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -36);

    lv_obj_t *dot = lv_obj_create(scr);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xE8A44A), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(dot, 80, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, 196);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dot);
    lv_anim_set_values(&a, 60, 255);
    lv_anim_set_time(&a, 650);
    lv_anim_set_playback_time(&a, 650);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, boot_dot_opa_cb);
    lv_anim_start(&a);

    lv_disp_load_scr(scr);
}

static void boot_tone_task(void *arg)
{
    (void)arg;
    s_boot_tone_ret = audio_backend_play_startup_tone();
    s_boot_tone_done = true;
    vTaskDelete(NULL);
}

static void hw_shutdown_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Shutdown sequence start");

    esp_err_t ret = audio_backend_prepare_shutdown();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "audio_backend_prepare_shutdown returned %s", esp_err_to_name(ret));
    }

    /* Give codec/amp line a short settle window before cutting power. */
    vTaskDelay(pdMS_TO_TICKS(80));

    gpio_set_level(HW_POWER_OFF_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(HW_POWER_OFF_PULSE_MS));
    gpio_set_level(HW_POWER_OFF_GPIO, 0);

    s_shutdown_task_started = false;
    vTaskDelete(NULL);
}

static void hw_request_power_off(void)
{
    if (s_shutdown_task_started) {
        return;
    }
    s_shutdown_task_started = true;

    BaseType_t ok = xTaskCreatePinnedToCore(
        hw_shutdown_task,
        "shutdown",
        6144,
        NULL,
        5,
        NULL,
        0
    );
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "shutdown task create failed");
        s_shutdown_task_started = false;
    }
}

static void hw_buttons_init(void)
{
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << HW_SLEEP_BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_cfg));

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << HW_POWER_OFF_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_cfg));
    ESP_ERROR_CHECK(gpio_set_level(HW_POWER_OFF_GPIO, 0));

    bool raw_pressed = (gpio_get_level(HW_SLEEP_BTN_GPIO) == 0);
    s_hw_sleep_raw_prev = raw_pressed;
    s_hw_sleep_stable = raw_pressed;
    s_hw_sleep_last_change_ms = lv_tick_get();
    s_hw_sleep_press_start_ms = s_hw_sleep_last_change_ms;
    s_hw_sleep_long_handled = false;

    ESP_LOGI(TAG,
             "HW buttons ready: sleep_in=GPIO%d power_off_out=GPIO%d",
             (int)HW_SLEEP_BTN_GPIO,
             (int)HW_POWER_OFF_GPIO);
}

static void hw_buttons_poll_cb(lv_timer_t *t)
{
    (void)t;
    const uint32_t now = lv_tick_get();
    const bool raw_pressed = (gpio_get_level(HW_SLEEP_BTN_GPIO) == 0);

    if (raw_pressed != s_hw_sleep_raw_prev) {
        s_hw_sleep_raw_prev = raw_pressed;
        s_hw_sleep_last_change_ms = now;
    }

    if ((now - s_hw_sleep_last_change_ms) >= HW_BTN_DEBOUNCE_MS &&
        raw_pressed != s_hw_sleep_stable) {
        s_hw_sleep_stable = raw_pressed;

        if (s_hw_sleep_stable) {
            ui_wakeup();
            s_hw_sleep_press_start_ms = now;
            s_hw_sleep_long_handled = false;
        } else {
            if (!s_hw_sleep_long_handled) {
                ui_app_cycle_sleep_timer();
            }
        }
    }

    if (s_hw_sleep_stable &&
        !s_hw_sleep_long_handled &&
        (now - s_hw_sleep_press_start_ms) >= HW_BTN_LONG_PRESS_MS) {
        s_hw_sleep_long_handled = true;
        ui_toast_show("Power off...");
        ui_app_request_shutdown();
    }
}

static void run_boot_sequence(void)
{
    boot_show_screen();
    boot_pump_ui_ms(120);

    s_boot_tone_done = false;
    s_boot_tone_ret = ESP_FAIL;

    if (!ui_app_get_startup_sound_enabled()) {
        s_boot_tone_done = true;
        s_boot_tone_ret = ESP_OK;
        boot_pump_ui_ms(80);
        return;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        boot_tone_task,
        "boot_tone",
        6144,
        NULL,
        5,
        NULL,
        1
    );

    if (created != pdPASS) {
        ESP_LOGW(TAG, "boot_tone task create failed; running tone inline");
        s_boot_tone_ret = audio_backend_play_startup_tone();
        s_boot_tone_done = true;
    }

    uint32_t start = lv_tick_get();
    while (!s_boot_tone_done && lv_tick_elaps(start) < 2500) {
        boot_pump_ui_ms(20);
    }

    if (!s_boot_tone_done) {
        ESP_LOGW(TAG, "boot_tone timeout");
    } else if (s_boot_tone_ret != ESP_OK) {
        ESP_LOGW(TAG, "startup tone skipped (%s)", esp_err_to_name(s_boot_tone_ret));
    }

    boot_pump_ui_ms(80);
}


/* ------------------------------------------------------------------ */
/*  Wakeup                                                              */
/* ------------------------------------------------------------------ */
void ui_wakeup(void)
{
    s_last_activity = lv_tick_get();

    if (s_idle_state != UI_ACTIVE) {
        screensaver_destroy();
        ui_set_backlight_target(s_user_brightness);
        s_idle_state = UI_ACTIVE;
    }
}

/* ------------------------------------------------------------------ */
/*  Backlight Control                                                   */
/* ------------------------------------------------------------------ */
void ui_set_backlight_target(uint8_t pct)
{
    if (pct > 100) pct = 100;
    s_bl_target = pct;
}

void ui_set_user_brightness(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    s_user_brightness = pct;
    if (s_idle_state == UI_ACTIVE) {
        ui_set_backlight_target(pct);
    }
}

uint8_t ui_get_user_brightness(void)
{
    return s_user_brightness;
}

void ui_set_min_brightness(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    s_min_brightness = pct;
}

uint8_t ui_get_min_brightness(void)
{
    return s_min_brightness;
}

void ui_set_dim_after_seconds(uint16_t seconds)
{
    if (seconds < 5u) {
        seconds = 5u;
    }
    s_dim_after_ms = (uint32_t)seconds * 1000u;
    s_saver_after_ms = s_dim_after_ms + 10000u;
}

uint16_t ui_get_dim_after_seconds(void)
{
    return (uint16_t)(s_dim_after_ms / 1000u);
}

void ui_set_idle_off_minutes(uint16_t minutes)
{
    if (minutes < 1u) {
        minutes = 1u;
    }
    s_idle_off_ms = (uint32_t)minutes * 60u * 1000u;
}

uint16_t ui_get_idle_off_minutes(void)
{
    return (uint16_t)(s_idle_off_ms / (60u * 1000u));
}

/* ------------------------------------------------------------------ */
/*  Screensaver                                                         */
/* ------------------------------------------------------------------ */
static void screensaver_vu_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_screensaver || !s_vu_wrap) {
        return;
    }

    const uint32_t now = lv_tick_get();
    const int bar_w = 14;
    const int bar_gap = 6;
    const int bar_max_h = 220;
    const int bar_min_h = 12;
    uint8_t vu_raw = 0;
    if (audio_backend_get_vu_level(&vu_raw) != ESP_OK) {
        vu_raw = 0;
    }
    float vu_env = 0.08f + 0.92f * ((float)vu_raw / 100.0f);

    for (int i = 0; i < 22; i++) {
        lv_obj_t *bar = s_vu_bars[i];
        if (!bar) {
            continue;
        }

        float a = (sinf((float)now * 0.006f + (float)i * 0.55f) + 1.0f) * 0.5f;
        float b = (sinf((float)now * 0.011f + (float)i * 1.37f + 1.2f) + 1.0f) * 0.5f;
        float c = (sinf((float)now * 0.017f + (float)i * 0.23f + 2.4f) + 1.0f) * 0.5f;
        float level = vu_env * (0.28f + 0.72f * (0.55f * a + 0.30f * b + 0.15f * c));
        int h = bar_min_h + (int)(level * (float)(bar_max_h - bar_min_h));

        lv_obj_set_size(bar, bar_w, h);
        lv_obj_set_pos(bar, i * (bar_w + bar_gap), bar_max_h - h);
    }
}

static void screensaver_create(void)
{
    if (s_screensaver) return;

    s_screensaver = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_screensaver);
    lv_obj_set_size(s_screensaver, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_screensaver, lv_color_black(), 0);

    /* title */
    lv_obj_t *label = lv_label_create(s_screensaver);
    lv_label_set_text(label, "Now Playing");
    lv_obj_set_style_text_color(label, lv_color_hex(0xE8F1FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_30, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 88);

    /* colorful VU meter */
    s_vu_wrap = lv_obj_create(s_screensaver);
    lv_obj_set_size(s_vu_wrap, 434, 240);
    lv_obj_align(s_vu_wrap, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_set_style_bg_color(s_vu_wrap, lv_color_hex(0x0F1118), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_vu_wrap, LV_OPA_80, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_vu_wrap, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_vu_wrap, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_vu_wrap, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(s_vu_wrap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 22; i++) {
        lv_obj_t *bar = lv_obj_create(s_vu_wrap);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 14, 16);
        lv_obj_set_pos(bar, i * (14 + 6), 220 - 16);
        lv_obj_set_style_radius(bar, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        uint16_t hue = (uint16_t)((i * 290) / 22);
        lv_color_t col = lv_color_hsv_to_rgb(hue, 92, 100);
        lv_obj_set_style_bg_color(bar, col, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(bar, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_color(bar, col, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_opa(bar, LV_OPA_20, LV_PART_MAIN | LV_STATE_DEFAULT);
        s_vu_bars[i] = bar;
    }

    screensaver_vu_tick(NULL);
    s_saver_vu_timer = lv_timer_create(screensaver_vu_tick, SAVER_VU_TIMER_MS, NULL);
}

static void screensaver_destroy(void)
{
    if (!s_screensaver) return;

    if (s_saver_vu_timer) {
        lv_timer_del(s_saver_vu_timer);
        s_saver_vu_timer = NULL;
    }
    s_vu_wrap = NULL;
    for (int i = 0; i < 22; i++) {
        s_vu_bars[i] = NULL;
    }

    lv_obj_del(s_screensaver);
    s_screensaver = NULL;
}

/* ------------------------------------------------------------------ */
/*  Idle Check                                                          */
/* ------------------------------------------------------------------ */
static void idle_check_cb(lv_timer_t *t)
{
    uint32_t now  = lv_tick_get();
    uint32_t idle = now - s_last_activity;
    const spod_app_state_t *state = ui_app_get_state();
    const bool is_playing = (state && state->playback_state == PLAYBACK_PLAYING);

    if (!is_playing && idle > s_idle_off_ms) {
        ui_toast_show("Idle power off");
        ui_app_request_shutdown();
        return;
    }

    switch (s_idle_state) {

        case UI_ACTIVE:
            if (idle > s_dim_after_ms) {
                ui_set_backlight_target(s_min_brightness);
                s_idle_state = UI_DIMMED;
            }
            break;

        case UI_DIMMED:
            if (idle > s_saver_after_ms) {
                if (audio_backend_is_playback_active()) {
                    screensaver_create();
                    ui_set_backlight_target(s_min_brightness / 2u);
                    s_idle_state = UI_SCREENSAVER;
                }
            }
            break;

        case UI_SCREENSAVER:
            if (!audio_backend_is_playback_active()) {
                screensaver_destroy();
                ui_set_backlight_target(s_min_brightness);
                s_idle_state = UI_DIMMED;
            }
            break;
    }
}

/* ------------------------------------------------------------------ */
/*  Backlight Animation                                                 */
/* ------------------------------------------------------------------ */
static void backlight_anim_cb(lv_timer_t *t)
{
    if (s_bl_current == s_bl_target) return;

    if (s_bl_current > s_bl_target)
        s_bl_current -= 2;
    else
        s_bl_current += 2;

    bsp_backlight_set(s_bl_current);
}

static void playback_progress_cb(lv_timer_t *t)
{
    (void)t;
    if (s_idle_state == UI_SCREENSAVER) {
        return;
    }
    ui_app_poll_playback_progress();
}

/* ------------------------------------------------------------------ */
/*  Main Application                                                    */
/* ------------------------------------------------------------------ */
void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "sPod firmware v%s",
             (app_desc && app_desc->version[0]) ? app_desc->version : "unknown");

#if defined(CONFIG_SPOD_C6_DEBUG_BRIDGE) && CONFIG_SPOD_C6_DEBUG_BRIDGE
    c6_debug_bridge_run();
#endif

    bsp_display_init();
    hw_buttons_init();

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };

    esp_timer_handle_t tick_timer;
    esp_timer_create(&tick_args, &tick_timer);
    esp_timer_start_periodic(tick_timer, 1000);

    ui_app_load_persisted_settings();
    run_boot_sequence();
    ui_app_init();
    ui_app_set_shutdown_callback(hw_request_power_off);

    lv_timer_create(backlight_anim_cb, 20, NULL);
    lv_timer_create(playback_progress_cb, 250, NULL);
    lv_timer_create(idle_check_cb, 1000, NULL); // check la 1 sec
    lv_timer_create(hw_buttons_poll_cb, HW_BTN_POLL_MS, NULL);
    while (1) {
        uint32_t delay = lv_timer_handler();
        //lv_refr_now(NULL);   // <-- AICI, forÈ›Äƒm reÃ®mprospÄƒtarea ecranului imediat dupÄƒ procesarea timerelor
        vTaskDelay(pdMS_TO_TICKS(delay ? delay : 1));
    }
}

