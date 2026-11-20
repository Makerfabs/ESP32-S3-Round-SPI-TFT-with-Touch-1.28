#include "board_storage.h"

#include "board_config.h"
#include "map_tile_cache.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "board_storage";

esp_err_t board_storage_mount(void)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = BOARD_SD_MOSI,
        .miso_io_num = BOARD_SD_MISO,
        .sclk_io_num = BOARD_SD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16 * 1024,
    };
    esp_err_t err = spi_bus_initialize(BOARD_SD_SPI_HOST, &bus_config, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SD_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = BOARD_SD_CS;
    slot.host_id = (spi_host_device_t)host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdspi_mount(BOARD_SD_MOUNT_POINT, &host, &slot,
                                  &mount_config, &card);
    if (err == ESP_OK) {
        (void)card;
    } else {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
    }
    return err;
}

static bool is_zoom_directory_name(const char *name)
{
    if (name[0] < '0' || name[0] > '9') return false;
    char *end = NULL;
    long zoom = strtol(name, &end, 10);
    return end != name && *end == '\0' && zoom <= 22;
}

static bool holds_zoom_levels(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return false;
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)) != NULL) {
        found = is_zoom_directory_name(entry->d_name);
    }
    closedir(dir);
    return found;
}

int board_storage_scan_map_folders(board_storage_map_t *out, int max_entries)
{
    if (!out || max_entries <= 0) return 0;
    DIR *root = opendir(BOARD_SD_MOUNT_POINT);
    if (!root) {
        ESP_LOGE(TAG, "cannot list %s", BOARD_SD_MOUNT_POINT);
        return 0;
    }

    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(root)) != NULL && found < max_entries) {
        if (entry->d_name[0] == '.' || strlen(entry->d_name) >= BOARD_STORAGE_TILE_FOLDER_LEN) continue;
        char folder_path[192];
        int length = snprintf(folder_path, sizeof(folder_path), "%s/%s",
                              BOARD_SD_MOUNT_POINT, entry->d_name);
        if (length < 0 || length >= (int)sizeof(folder_path) ||
            !holds_zoom_levels(folder_path)) continue;
        int min_zoom = 0;
        int max_zoom = 0;
        if (map_tile_cache_probe_zooms(BOARD_SD_MOUNT_POINT, entry->d_name,
                                       &min_zoom, &max_zoom) <= 0) {
            continue;
        }
        memset(&out[found], 0, sizeof(out[found]));
        snprintf(out[found].folder, sizeof(out[found].folder), "%.*s",
                 (int)sizeof(out[found].folder) - 1, entry->d_name);
        out[found].min_zoom = (uint8_t)min_zoom;
        out[found].max_zoom = (uint8_t)max_zoom;
        ++found;
    }
    closedir(root);
    return found;
}

