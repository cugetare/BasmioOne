idf_component_register(
    SRCS "bsp_display.c" "main.c" "c6_debug_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES ui lvgl
    PRIV_REQUIRES esp_lcd esp_timer esp_app_format esp_lcd_touch_gt911 esp_lcd_st7701 esp_driver_gpio esp_driver_uart
    EMBED_FILES "assets/boot_ro_basmio.png" "assets/boot_en_basmio.png"
)
