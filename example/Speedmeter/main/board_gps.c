#include "board_gps.h"

#include "board_config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define GPS_RX_BUFFER_SIZE 2048
#define GPS_LINE_MAX       160
#define GPS_FIX_STALE_MS   3000

static TaskHandle_t s_rx_task;
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static board_gps_status_t s_status;
static const char *TAG = "board_gps";
static uint32_t s_rate_start_ms;
static unsigned s_rmc_count;
static bool s_rate_reported;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool nmea_checksum_valid(const char *line)
{
    if (!line || line[0] != '$') return false;
    const char *star = strchr(line, '*');
    if (!star || !star[1] || !star[2]) return false;

    int high = hex_value(star[1]);
    int low = hex_value(star[2]);
    if (high < 0 || low < 0) return false;

    uint8_t checksum = 0;
    for (const char *p = line + 1; p < star; ++p) checksum ^= (uint8_t)*p;
    return checksum == (uint8_t)((high << 4) | low);
}

/* Probe checksummed output, including no-fix sentences: no sky view needed. */
static bool wait_for_nmea(uint32_t timeout_ms)
{
    char line[GPS_LINE_MAX];
    size_t length = 0;
    uint32_t start = now_ms();
    while (now_ms() - start < timeout_ms) {
        uint8_t bytes[128];
        int count = uart_read_bytes(BOARD_GPS_UART_PORT, bytes, sizeof(bytes),
                                    pdMS_TO_TICKS(50));
        for (int i = 0; i < count; ++i) {
            char c = (char)bytes[i];
            if (c == '$') {
                length = 0;
                line[length++] = c;
            } else if (c == '\n' && length > 0) {
                line[length] = '\0';
                if (nmea_checksum_valid(line) && length >= 7 &&
                    (memcmp(line + 3, "GGA,", 4) == 0 ||
                     memcmp(line + 3, "RMC,", 4) == 0)) return true;
                length = 0;
            } else if (c != '\r' && length > 0) {
                if (length < sizeof(line) - 1) line[length++] = c;
                else length = 0;
            }
        }
    }
    return false;
}

static int detect_baud(void)
{
    const int rates[] = {BOARD_GPS_DEFAULT_BAUD, BOARD_GPS_UART_BAUD,
                         38400, 57600, 19200, 4800};
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        if (uart_set_baudrate(BOARD_GPS_UART_PORT, rates[i]) != ESP_OK) continue;
        uart_flush_input(BOARD_GPS_UART_PORT);
        if (wait_for_nmea(1600)) return rates[i];
    }
    return 0;
}

static bool send_pcas(const char *body)
{
    uint8_t checksum = 0;
    for (const char *p = body; *p; ++p) checksum ^= (uint8_t)*p;
    char command[96];
    int length = snprintf(command, sizeof(command), "$%s*%02X\r\n", body,
                          (unsigned)checksum);
    if (length <= 0 || length >= (int)sizeof(command)) return false;
    if (uart_write_bytes(BOARD_GPS_UART_PORT, command, length) != length) return false;
    return uart_wait_tx_done(BOARD_GPS_UART_PORT, pdMS_TO_TICKS(500)) == ESP_OK;
}

static bool configure_receiver(void)
{
    int baud = detect_baud();
    if (!baud) {
        /* Keep looking in this task; the UI can start even if GPS is absent. */
        ESP_LOGW(TAG, "no NMEA detected; retrying baud detection");
        return false;
    }
    ESP_LOGI(TAG, "receiver detected at %d baud", baud);
    if (baud != BOARD_GPS_UART_BAUD) {
        /* PCAS01 index 5 means 115200. Wait for TX before changing ESP UART. */
        if (!send_pcas("PCAS01,5")) {
            ESP_LOGW(TAG, "baud command send failed; keeping %d baud", baud);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        uart_set_baudrate(BOARD_GPS_UART_PORT, BOARD_GPS_UART_BAUD);
        uart_flush_input(BOARD_GPS_UART_PORT);
        if (!wait_for_nmea(2000)) {
            ESP_LOGW(TAG, "115200 baud not verified; detecting receiver again");
            baud = detect_baud();
            if (baud != BOARD_GPS_UART_BAUD) {
                ESP_LOGW(TAG, "5 Hz configuration skipped; detected baud=%d", baud);
                return baud != 0;
            }
        }
    }
    char rate_command[32];
    snprintf(rate_command, sizeof(rate_command), "PCAS02,%u",
             (unsigned)BOARD_GPS_FIX_INTERVAL_MS);
    if (send_pcas(rate_command)) {
        ESP_LOGI(TAG, "requested 5 Hz at 115200 baud; awaiting measured RMC rate");
    } else {
        ESP_LOGW(TAG, "5 Hz command send failed; continuing reception");
    }
    return true;
}

static bool parse_coordinate(const char *value, const char *hemisphere,
                             bool latitude, double *degrees)
{
    if (!value || !hemisphere || !degrees || value[0] == '\0' ||
        hemisphere[0] == '\0') return false;

    char *end = NULL;
    double raw = strtod(value, &end);
    if (end == value || *end != '\0') return false;

    int whole_degrees = (int)(raw / 100.0);
    double minutes = raw - (double)whole_degrees * 100.0;
    if (minutes < 0.0 || minutes >= 60.0) return false;

    double result = whole_degrees + minutes / 60.0;
    char direction = hemisphere[0];
    if (latitude) {
        if (direction != 'N' && direction != 'S') return false;
        if (result > 90.0) return false;
        if (direction == 'S') result = -result;
    } else {
        if (direction != 'E' && direction != 'W') return false;
        if (result > 180.0) return false;
        if (direction == 'W') result = -result;
    }
    *degrees = result;
    return true;
}

static int split_fields(char *line, char *fields[], int capacity)
{
    int count = 0;
    char *field_start = line;
    for (char *p = line; *p && *p != '*'; ++p) {
        if (*p == ',') {
            *p = '\0';
            if (count < capacity) fields[count++] = field_start;
            field_start = p + 1;
        }
    }
    if (count < capacity) fields[count++] = field_start;
    return count;
}

static void parse_gga(char *fields[], int count)
{
    if (count < 8) return;
    int quality = atoi(fields[6]);
    int satellites = atoi(fields[7]);
    portENTER_CRITICAL(&s_status_mux);
    s_status.satellites = (uint8_t)(satellites < 0 ? 0 : satellites);
    /* RMC alone publishes each navigation sample, with position and speed
     * from the same sentence; GGA must not publish a second partial sample. */
    if (quality <= 0) s_status.fix_valid = false;
    portEXIT_CRITICAL(&s_status_mux);
}

static void parse_rmc(char *fields[], int count, uint32_t timestamp)
{
    if (count < 9) return;
    double latitude = 0.0;
    double longitude = 0.0;
    bool coordinates_valid =
        parse_coordinate(fields[3], fields[4], true, &latitude) &&
        parse_coordinate(fields[5], fields[6], false, &longitude);
    bool fix_valid = fields[2][0] == 'A' && coordinates_valid;
    float speed_kmh = fields[7][0] ? strtof(fields[7], NULL) * 1.852f : 0.0f;
    float course_deg = fields[8][0] ? strtof(fields[8], NULL) : 0.0f;

    portENTER_CRITICAL(&s_status_mux);
    s_status.fix_valid = fix_valid;
    if (fix_valid) {
        s_status.latitude = latitude;
        s_status.longitude = longitude;
        s_status.speed_kmh = speed_kmh;
        s_status.course_deg = course_deg;
        s_status.last_fix_ms = timestamp;
    }
    portEXIT_CRITICAL(&s_status_mux);
}

static void process_sentence(char *line)
{
    if (!nmea_checksum_valid(line)) {
        return;
    }

    char *fields[20];
    int count = split_fields(line, fields, 20);
    if (count == 0 || fields[0][0] != '$' || strlen(fields[0]) < 6) return;

    uint32_t timestamp = now_ms();
    const char *kind = fields[0] + strlen(fields[0]) - 3;
    if (strcmp(kind, "GGA") == 0) {
        parse_gga(fields, count);
    } else if (strcmp(kind, "RMC") == 0) {
        parse_rmc(fields, count, timestamp);
        /* One startup measurement, also works indoors with invalid fixes. */
        if (!s_rate_reported && count >= 9) {
            if (s_rmc_count++ == 0) s_rate_start_ms = timestamp;
            uint32_t elapsed = timestamp - s_rate_start_ms;
            if (elapsed >= 5000) {
                unsigned rate_tenths = (s_rmc_count - 1) * 10000U / elapsed;
                ESP_LOGI(TAG, "measured RMC output: %u.%u Hz (target 5 Hz)",
                         rate_tenths / 10, rate_tenths % 10);
                if (rate_tenths < 40 || rate_tenths > 60) {
                    ESP_LOGW(TAG, "unexpected RMC rate; check receiver configuration");
                }
                s_rate_reported = true;
            }
        }
    }
}

static void gps_rx_task(void *arg)
{
    (void)arg;
    while (!configure_receiver()) vTaskDelay(pdMS_TO_TICKS(500));
    uint8_t bytes[128];
    char line[GPS_LINE_MAX];
    size_t line_length = 0;
    while (true) {
        int received = uart_read_bytes(BOARD_GPS_UART_PORT, bytes, sizeof(bytes),
                                       pdMS_TO_TICKS(20));
        for (int i = 0; i < received; ++i) {
            char c = (char)bytes[i];
            if (c == '$') {
                line_length = 0;
                line[line_length++] = c;
            } else if (c == '\n') {
                if (line_length > 0) {
                    line[line_length] = '\0';
                    process_sentence(line);
                    line_length = 0;
                }
            } else if (c != '\r' && line_length > 0) {
                if (line_length < sizeof(line) - 1) {
                    line[line_length++] = c;
                } else {
                    line_length = 0;
                }
            }
        }

        uint32_t now = now_ms();
        board_gps_status_t status;
        board_gps_get_status(&status);
        if (status.fix_valid && now - status.last_fix_ms > GPS_FIX_STALE_MS) {
            portENTER_CRITICAL(&s_status_mux);
            s_status.fix_valid = false;
            portEXIT_CRITICAL(&s_status_mux);
        }

    }
}

esp_err_t board_gps_start(void)
{
    if (s_rx_task) return ESP_OK;

    const uart_config_t config = {
        .baud_rate = BOARD_GPS_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(BOARD_GPS_UART_PORT,
                                        GPS_RX_BUFFER_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;
    err = uart_param_config(BOARD_GPS_UART_PORT, &config);
    if (err != ESP_OK) {
        uart_driver_delete(BOARD_GPS_UART_PORT);
        return err;
    }
    err = uart_set_pin(BOARD_GPS_UART_PORT, BOARD_GPS_UART_TX_PIN,
                       BOARD_GPS_UART_RX_PIN, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        uart_driver_delete(BOARD_GPS_UART_PORT);
        return err;
    }

    BaseType_t created = xTaskCreate(gps_rx_task, "gps_rx", 4096, NULL, 5,
                                     &s_rx_task);
    if (created != pdPASS) {
        s_rx_task = NULL;
        uart_driver_delete(BOARD_GPS_UART_PORT);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool board_gps_get_status(board_gps_status_t *status)
{
    if (!status) return false;
    portENTER_CRITICAL(&s_status_mux);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_mux);
    return true;
}
