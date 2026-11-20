#pragma once

#include <stdint.h>
#include "esp_err.h"

#define BOARD_STORAGE_MAX_MAPS 24
#define BOARD_STORAGE_TILE_FOLDER_LEN 32

typedef struct {
    char folder[BOARD_STORAGE_TILE_FOLDER_LEN];
    uint8_t min_zoom;
    uint8_t max_zoom;
} board_storage_map_t;

esp_err_t board_storage_mount(void);
int board_storage_scan_map_folders(board_storage_map_t *out, int max_entries);
