#include "board_display.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_color_done;

static bool color_trans_done_cb(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *event,
                                void *user_ctx)
{
    (void)io;
    (void)event;
    SemaphoreHandle_t done = user_ctx;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(done, &task_woken);
    return task_woken == pdTRUE;
}

esp_err_t board_display_init(void)
{
    gpio_config_t backlight = {
        .pin_bit_mask = 1ULL << BOARD_LCD_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&backlight);
    if (err != ESP_OK) return err;
    gpio_set_level(BOARD_LCD_BACKLIGHT, 0);
    s_color_done = xSemaphoreCreateBinary();
    if (!s_color_done) return ESP_ERR_NO_MEM;

    const spi_bus_config_t bus_config = {
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = BOARD_LCD_MISO,
        .sclk_io_num = BOARD_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_WIDTH * 40 * sizeof(uint16_t),
    };
    err = spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BOARD_LCD_DC,
        .cs_gpio_num = BOARD_LCD_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = color_trans_done_cb,
        .user_ctx = s_color_done,
        .flags = { .dc_low_on_data = 0, .octal_mode = 0, .quad_mode = 0,
                   .sio_mode = 0, .lsb_first = 0, .cs_high_active = 0 },
    };
    esp_lcd_panel_io_handle_t io;
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST,
                                    &io_config, &io);
    if (err != ESP_OK) return err;

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_gc9a01(io, &panel_config, &s_panel);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) return err;
    /* Correct the panel's observed left-right mirrored scan direction. */
    err = esp_lcd_panel_mirror(s_panel, true, false);
    if (err != ESP_OK) return err;
    /* Match the Makerfabs Arduino_GC9A01 IPS initialization. */
    err = esp_lcd_panel_invert_color(s_panel, true);
    if (err != ESP_OK) return err;
    err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) return err;

    gpio_set_level(BOARD_LCD_BACKLIGHT, 1);
    return ESP_OK;
}

esp_err_t board_display_flush_pixels(int x1, int y1, int x2, int y2,
                                     void *pixels, size_t pixel_count)
{
    if (!s_panel || !s_color_done || !pixels || x1 < 0 || y1 < 0 ||
        x2 <= x1 || y2 <= y1 || x2 > BOARD_LCD_WIDTH || y2 > BOARD_LCD_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *bytes = pixels;
    for (size_t i = 0; i < pixel_count; ++i) {
        uint8_t tmp = bytes[i * 2];
        bytes[i * 2] = bytes[i * 2 + 1];
        bytes[i * 2 + 1] = tmp;
    }

    xSemaphoreTake(s_color_done, 0);
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, pixels);
    if (err == ESP_OK) {
        xSemaphoreTake(s_color_done, portMAX_DELAY);
    }

    for (size_t i = 0; i < pixel_count; ++i) {
        uint8_t tmp = bytes[i * 2];
        bytes[i * 2] = bytes[i * 2 + 1];
        bytes[i * 2 + 1] = tmp;
    }
    return err;
}

