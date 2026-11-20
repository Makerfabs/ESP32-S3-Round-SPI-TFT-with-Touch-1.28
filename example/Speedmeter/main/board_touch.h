#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool pressed;
    unsigned x;
    unsigned y;
} board_touch_point_t;

esp_err_t board_touch_init(void);
esp_err_t board_touch_read(board_touch_point_t *point);
