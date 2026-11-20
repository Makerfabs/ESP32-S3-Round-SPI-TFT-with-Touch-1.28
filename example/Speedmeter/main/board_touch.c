#include "board_touch.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CST816S_REG_GESTURE_ID 0x01

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_write_read_device(BOARD_TOUCH_I2C_PORT,
                                        BOARD_TOUCH_I2C_ADDRESS,
                                        &reg, 1, data, length,
                                        pdMS_TO_TICKS(50));
}

esp_err_t board_touch_init(void)
{
    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BOARD_TOUCH_SDA,
        .scl_io_num = BOARD_TOUCH_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t err = i2c_param_config(BOARD_TOUCH_I2C_PORT, &config);
    if (err != ESP_OK) return err;
    err = i2c_driver_install(BOARD_TOUCH_I2C_PORT, config.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    gpio_config_t reset = {
        .pin_bit_mask = 1ULL << BOARD_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&reset));
    gpio_set_level(BOARD_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}

esp_err_t board_touch_read(board_touch_point_t *point)
{
    if (!point) return ESP_ERR_INVALID_ARG;
    /* CST816S register block: gesture, finger count, X high/low, Y high/low. */
    uint8_t data[6] = {0};
    esp_err_t err = read_regs(CST816S_REG_GESTURE_ID, data, sizeof(data));
    if (err != ESP_OK) return err;

    point->pressed = (data[1] & 0x0F) != 0;
    point->x = ((unsigned)(data[2] & 0x0F) << 8) | data[3];
    point->y = ((unsigned)(data[4] & 0x0F) << 8) | data[5];
    if (point->x >= BOARD_LCD_WIDTH || point->y >= BOARD_LCD_HEIGHT) {
        point->pressed = false;
    }
    return ESP_OK;
}
