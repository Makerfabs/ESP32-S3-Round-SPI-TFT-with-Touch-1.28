idf_component_register(
    SRCS "nav_ui.c" "nav_engine.c" "board_gps.c" "main.c" "board_display.c" "board_touch.c" "board_storage.c" "board_lvgl.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_uart esp_lcd esp_lcd_gc9a01 esp_timer fatfs lvgl map_tiles sdmmc
)
