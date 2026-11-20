#include "board_display.h"
#include "board_gps.h"
#include "board_lvgl.h"
#include "board_storage.h"
#include "board_touch.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "speedmeter";

void app_main(void)
{
    ESP_ERROR_CHECK(board_display_init());

    esp_err_t touch_err = board_touch_init();
    if (touch_err != ESP_OK) {
        ESP_LOGE(TAG, "touch unavailable: %s", esp_err_to_name(touch_err));
    }

    esp_err_t storage_err = board_storage_mount();
    if (storage_err != ESP_OK) {
        ESP_LOGE(TAG, "TF card unavailable: %s", esp_err_to_name(storage_err));
    }

    esp_err_t gps_err = board_gps_start();
    if (gps_err != ESP_OK) {
        ESP_LOGE(TAG, "GPS receiver unavailable: %s", esp_err_to_name(gps_err));
    }

    ESP_ERROR_CHECK(board_lvgl_start(storage_err == ESP_OK));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
