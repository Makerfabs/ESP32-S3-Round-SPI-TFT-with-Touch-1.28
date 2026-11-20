#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool fix_valid;
    uint8_t satellites;
    double latitude;
    double longitude;
    float speed_kmh;
    float course_deg;
    uint32_t last_fix_ms;
} board_gps_status_t;

/** Start UART2 reception; detect receiver baud and request 115200 baud / 5 Hz. */
esp_err_t board_gps_start(void);

/** Copy the latest receiver status. */
bool board_gps_get_status(board_gps_status_t *status);
