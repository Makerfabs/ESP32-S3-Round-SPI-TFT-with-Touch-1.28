#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t board_display_init(void);
esp_err_t board_display_flush_pixels(int x1, int y1, int x2, int y2,
                                     void *pixels, size_t pixel_count);
