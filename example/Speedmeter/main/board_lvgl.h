#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Start the UI; the SD card may be mounted even if route validation failed. */
esp_err_t board_lvgl_start(bool storage_ready);
