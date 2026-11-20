#include "board_lvgl.h"

#include "board_config.h"
#include "board_display.h"
#include "board_gps.h"
#include "board_storage.h"
#include "board_touch.h"
#include "nav_engine.h"
#include "nav_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "map_geo.h"
#include "map_route.h"
#include "map_tile_cache.h"
#include "map_view.h"

#define LVGL_BUFFER_LINES 40
#define TOUCH_POLL_MS 20
#define MAX_ROUTES 24
#define ROUTES_DIR "/sdcard/routes"
#define SD_ROOT "/sdcard"
#define ROUTE_ROW_TEXT_X 26
#define NAV_FIX_MIN_INTERVAL_MS 200

static const char *TAG = "board_lvgl";
static lv_display_t *s_display;
static lv_obj_t *s_status_label;
static lv_obj_t *s_map;
static lv_obj_t *s_screen;
static map_route_handle_t s_route;
static nav_state_t s_nav_state;
typedef struct {
    char path[160];
    map_route_info_t info;
    bool map_only;
} route_entry_t;
static route_entry_t s_routes[MAX_ROUTES];
static int s_route_count;
static int s_entry_count;
static lv_timer_t *s_map_refresh_timer;
static uint32_t s_map_cache_version;
static lv_timer_t *s_gps_navigation_timer;
static uint32_t s_last_gps_fix_ms;
static bool s_gps_status_seen;
static bool s_last_fix_valid;
static bool s_have_nav_state;
static nav_phase_t s_last_logged_phase;
static bool s_have_logged_phase;
static void *s_draw_buffer;
static portMUX_TYPE s_touch_mux = portMUX_INITIALIZER_UNLOCKED;
static board_touch_point_t s_touch_point;

static bool build_static_map(lv_obj_t *screen, const char *route_path);
static bool build_map_only(lv_obj_t *screen, const route_entry_t *entry);
static void map_refresh_cb(lv_timer_t *timer);
static void gps_navigation_cb(lv_timer_t *timer);
static void route_picker_back_cb(lv_event_t *event);
static void navigation_session_cleanup(bool clear_screen);
static void build_route_picker(lv_obj_t *screen);
static void nav_change_route_cb(void *ctx);
static bool navigation_map_start(lv_obj_t *screen, const char *folder,
                                 int min_zoom, int max_zoom,
                                 map_route_handle_t route,
                                 bool map_only,
                                 bool fit_to_bounds,
                                 double south, double west,
                                 double north, double east);

static uint32_t lvgl_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void display_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    size_t pixel_count = (size_t)(area->x2 - area->x1 + 1) *
                         (size_t)(area->y2 - area->y1 + 1);
    esp_err_t err = board_display_flush_pixels(area->x1, area->y1,
                                                area->x2 + 1, area->y2 + 1,
                                                pixels, pixel_count);
    if (err != ESP_OK) ESP_LOGE(TAG, "display flush failed: %s", esp_err_to_name(err));
    lv_display_flush_ready(display);
}

static void touch_task(void *arg)
{
    (void)arg;
    while (true) {
        board_touch_point_t point = {0};
        esp_err_t err = board_touch_read(&point);
        portENTER_CRITICAL(&s_touch_mux);
        if (err == ESP_OK && point.pressed) s_touch_point = point;
        else s_touch_point.pressed = false;
        portEXIT_CRITICAL(&s_touch_mux);
        vTaskDelay(pdMS_TO_TICKS(TOUCH_POLL_MS));
    }
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    portENTER_CRITICAL(&s_touch_mux);
    board_touch_point_t point = s_touch_point;
    portEXIT_CRITICAL(&s_touch_mux);
    /* CST816S coordinates already match the corrected LCD orientation. */
    data->point.x = point.x;
    data->point.y = point.y;
    data->state = point.pressed
                      ? LV_INDEV_STATE_PRESSED
                      : LV_INDEV_STATE_RELEASED;
}

static bool has_bin_suffix(const char *name)
{
    size_t length = strlen(name);
    return length > 4 && strcasecmp(name + length - 4, ".bin") == 0;
}

static int scan_routes(void)
{
    DIR *dir = opendir(ROUTES_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "cannot open route directory: %s", ROUTES_DIR);
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_route_count < MAX_ROUTES) {
        if (entry->d_name[0] == '.' || !has_bin_suffix(entry->d_name)) continue;
        route_entry_t *route = &s_routes[s_route_count];
        int length = snprintf(route->path, sizeof(route->path), "%s/%s", ROUTES_DIR,
                              entry->d_name);
        if (length < 0 || length >= (int)sizeof(route->path) ||
            !map_route_peek(route->path, &route->info)) {
            ESP_LOGW(TAG, "skipping invalid route: %s", entry->d_name);
            continue;
        }
        if (route->info.name[0] == '\0') {
            snprintf(route->info.name, sizeof(route->info.name), "%.*s",
                     (int)sizeof(route->info.name) - 1, entry->d_name);
        }
        route->map_only = false;
        ++s_route_count;
    }
    closedir(dir);

    for (int i = 0; i < s_route_count; ++i) {
        for (int j = i + 1; j < s_route_count; ++j) {
            if (strcasecmp(s_routes[i].info.name, s_routes[j].info.name) > 0) {
                route_entry_t tmp = s_routes[i];
                s_routes[i] = s_routes[j];
                s_routes[j] = tmp;
            }
        }
    }
    return s_route_count;
}

static int scan_map_folders(void)
{
    board_storage_map_t maps[BOARD_STORAGE_MAX_MAPS];
    int available = board_storage_scan_map_folders(maps, BOARD_STORAGE_MAX_MAPS);
    int found = 0;
    for (int i = 0; i < available && s_entry_count < MAX_ROUTES; ++i) {
        route_entry_t *map = &s_routes[s_entry_count];
        memset(map, 0, sizeof(*map));
        int length = snprintf(map->path, sizeof(map->path), "%s/%s", SD_ROOT,
                              maps[i].folder);
        if (length < 0 || length >= (int)sizeof(map->path)) continue;
        snprintf(map->info.name, sizeof(map->info.name), "%s", maps[i].folder);
        snprintf(map->info.tile_folder, sizeof(map->info.tile_folder), "%s",
                 maps[i].folder);
        map->info.min_zoom = maps[i].min_zoom;
        map->info.max_zoom = maps[i].max_zoom;
        map->map_only = true;
        ++s_entry_count;
        ++found;
    }
    return found;
}

static bool parse_tile_index(const char *name, const char *suffix, long *value)
{
    if (name[0] < '0' || name[0] > '9') return false;
    char *end = NULL;
    long parsed = strtol(name, &end, 10);
    if (strcasecmp(end, suffix) != 0) return false;
    *value = parsed;
    return true;
}

static bool map_folder_bounds(const char *folder, int zoom,
                              double *south, double *west,
                              double *north, double *east)
{
    char zoom_path[192];
    int length = snprintf(zoom_path, sizeof(zoom_path), "%s/%s/%d",
                          SD_ROOT, folder, zoom);
    if (length < 0 || length >= (int)sizeof(zoom_path)) return false;
    DIR *zoom_dir = opendir(zoom_path);
    if (!zoom_dir) return false;

    long min_x = LONG_MAX, max_x = -1;
    long min_y = LONG_MAX, max_y = -1;
    struct dirent *x_entry;
    while ((x_entry = readdir(zoom_dir)) != NULL) {
        long x;
        if (!parse_tile_index(x_entry->d_name, "", &x)) continue;
        char x_path[224];
        length = snprintf(x_path, sizeof(x_path), "%s/%s", zoom_path, x_entry->d_name);
        if (length < 0 || length >= (int)sizeof(x_path)) continue;
        DIR *x_dir = opendir(x_path);
        if (!x_dir) continue;

        bool has_y = false;
        struct dirent *y_entry;
        while ((y_entry = readdir(x_dir)) != NULL) {
            long y;
            if (!parse_tile_index(y_entry->d_name, ".bin", &y)) continue;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
            has_y = true;
        }
        closedir(x_dir);
        if (has_y) {
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
        }
    }
    closedir(zoom_dir);
    if (max_x < 0 || max_y < 0) return false;

    const double tile_size = MAP_TILE_CACHE_TILE_SIZE;
    *west = map_geo_world_px_to_lon(min_x * tile_size, zoom);
    *east = map_geo_world_px_to_lon((max_x + 1) * tile_size, zoom);
    *north = map_geo_world_py_to_lat(min_y * tile_size, zoom);
    *south = map_geo_world_py_to_lat((max_y + 1) * tile_size, zoom);
    return true;
}

static void navigation_session_cleanup(bool clear_screen)
{
    /* The engine owns no route allocation, but its singleton still holds the
     * old handle. Clear it before the route file data is freed. */
    nav_engine_init(NULL);
    if (s_gps_navigation_timer) {
        lv_timer_delete(s_gps_navigation_timer);
        s_gps_navigation_timer = NULL;
    }
    if (s_map_refresh_timer) {
        lv_timer_delete(s_map_refresh_timer);
        s_map_refresh_timer = NULL;
    }
    nav_ui_destroy();
    if (s_map) map_view_set_route(s_map, NULL);
    if (clear_screen && s_screen) lv_obj_clean(s_screen);
    if (s_route) {
        map_route_free(s_route);
        s_route = NULL;
    }
    s_map = NULL;
    s_gps_status_seen = false;
    s_last_fix_valid = false;
    s_last_gps_fix_ms = 0;
    s_have_nav_state = false;
    s_have_logged_phase = false;
    memset(&s_nav_state, 0, sizeof(s_nav_state));
}

static bool navigation_map_start(lv_obj_t *screen, const char *folder,
                                 int min_zoom, int max_zoom,
                                 map_route_handle_t route,
                                 bool map_only,
                                 bool fit_to_bounds,
                                 double south, double west,
                                 double north, double east)
{
    if (!screen || !folder || min_zoom < 0 || max_zoom < min_zoom) return false;
    s_route = route;
    nav_map_extent_t extent = {south, west, north, east};
    bool created = map_only
        ? nav_ui_create_map_only(screen, folder, fit_to_bounds ? &extent : NULL)
        : nav_ui_create(screen, route);
    s_map = nav_ui_get_map();
    if (!created) return false;
    nav_ui_set_change_route_cb(nav_change_route_cb, NULL);

    if (route) nav_engine_init(route);
    else nav_engine_init_free();
    s_gps_navigation_timer = lv_timer_create(gps_navigation_cb,
                                             NAV_FIX_MIN_INTERVAL_MS, NULL);
    s_map_cache_version = map_tile_cache_get_version(map_view_get_tile_cache(s_map));
    s_map_refresh_timer = lv_timer_create(map_refresh_cb, 100, NULL);
    nav_engine_update(NULL, &s_nav_state);
    nav_ui_set_fix_valid(false);
    nav_ui_update(&s_nav_state);
    ESP_LOGI(TAG, "navigation map ready: %s%s z%d-%d",
             folder, map_only ? " (map only)" : "", min_zoom, max_zoom);
    return true;
}

static bool build_map_only(lv_obj_t *screen, const route_entry_t *entry)
{
    double south = 0.0, west = 0.0, north = 0.0, east = 0.0;
    bool have_bounds = map_folder_bounds(entry->info.tile_folder,
                                         entry->info.min_zoom,
                                         &south, &west, &north, &east);
    if (!have_bounds) {
        ESP_LOGW(TAG, "cannot determine map bounds for %s z%d",
                 entry->info.tile_folder, entry->info.min_zoom);
    }
    if (!navigation_map_start(screen, entry->info.tile_folder,
                              entry->info.min_zoom, entry->info.max_zoom,
                              NULL, true, have_bounds,
                              south, west, north, east)) {
        navigation_session_cleanup(true);
        return false;
    }
    ESP_LOGI(TAG, "opened map-only folder: %s z%d-%d",
             entry->info.tile_folder, entry->info.min_zoom,
             entry->info.max_zoom);
    return true;
}

static void route_picker_back_cb(lv_event_t *event)
{
    (void)event;
    navigation_session_cleanup(true);
    build_route_picker(s_screen);
}

static void route_row_clicked_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= s_entry_count) return;
    route_entry_t selected = s_routes[index];
    ESP_LOGI(TAG, "selected %s: %s",
             selected.map_only ? "map" : "route", selected.info.name);
    navigation_session_cleanup(true);
    bool opened = selected.map_only
                      ? build_map_only(s_screen, &selected)
                      : build_static_map(s_screen, selected.path);
    if (!opened) {
        navigation_session_cleanup(true);
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x101820), 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        lv_obj_t *error = lv_label_create(s_screen);
        lv_obj_set_style_text_color(error, lv_color_white(), 0);
        lv_label_set_text_fmt(error, "Cannot open %s\n%s",
                              selected.map_only ? "map" : "route",
                              selected.info.name);
        lv_obj_center(error);
    }
}

/* Shorten route titles for the small round display without changing SD data. */
static void format_route_display_name(const char *source, char *out, size_t size)
{
    if (size == 0) return;
    size_t used = 0;
    while (*source && used + 1 < size) {
        if (strncasecmp(source, " to ", 4) == 0) {
            out[used++] = '-';
            source += 4;
        } else {
            out[used++] = *source++;
        }
    }
    out[used] = '\0';
}

static void build_route_picker(lv_obj_t *screen)
{
    const lv_color_t cyan = lv_color_hex(0x18B9FF);
    unsigned map_count = 0;
    for (int i = 0; i < s_entry_count; ++i) map_count += s_routes[i].map_only;
    const char *title_text = map_count == 0 ? "Routes" :
                             map_count == (unsigned)s_entry_count ? "Maps" :
                             "Routes & maps";
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_label_set_text(title, title_text);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *caption = lv_label_create(screen);
    lv_obj_set_style_text_color(caption, cyan, 0);
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_12, 0);
    lv_label_set_text_fmt(caption, "%d on the card", s_entry_count);
    lv_obj_align(caption, LV_ALIGN_TOP_MID, 0, 37);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_set_size(list, BOARD_LCD_WIDTH - 20, BOARD_LCD_HEIGHT - 70);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 3, 0);
    lv_obj_set_style_pad_row(list, 7, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < s_entry_count; ++i) {
        lv_obj_t *row = lv_button_create(list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 64);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x03366F), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x07549A), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, cyan, 0);
        lv_obj_set_style_border_color(row, lv_color_white(), LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(row, 9, 0);
        lv_obj_set_style_pad_ver(row, 5, 0);
        lv_obj_set_layout(row, LV_LAYOUT_NONE);
        lv_obj_add_event_cb(row, route_row_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        char display_name[sizeof(s_routes[i].info.name)];
        format_route_display_name(s_routes[i].info.name, display_name,
                                  sizeof(display_name));
        lv_obj_t *icon = lv_label_create(row);
        lv_obj_set_style_text_color(icon, cyan, 0);
        const char *icon_symbol = s_routes[i].map_only ? LV_SYMBOL_IMAGE :
                                  s_routes[i].info.kind != MAP_ROUTE_KIND_EXERCISE ||
                                  strcasecmp(display_name, "Home-Company") == 0
                                      ? LV_SYMBOL_GPS : LV_SYMBOL_REFRESH;
        lv_label_set_text(icon, icon_symbol);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_width(name, 150);
        lv_obj_set_style_text_color(name, lv_color_white(), 0);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_label_set_text(name, display_name);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, ROUTE_ROW_TEXT_X, 2);

        lv_obj_t *meta = lv_label_create(row);
        lv_obj_set_width(meta, 174);
        lv_obj_set_style_text_color(meta, lv_color_hex(0x82D8FF), 0);
        lv_obj_set_style_text_font(meta, &lv_font_montserrat_12, 0);
        lv_label_set_long_mode(meta, LV_LABEL_LONG_CLIP);
        lv_obj_align(meta, LV_ALIGN_BOTTOM_LEFT, ROUTE_ROW_TEXT_X, -1);
        if (s_routes[i].map_only) {
            lv_label_set_text_fmt(meta, "map only " LV_SYMBOL_BULLET " z%u-%u",
                                  s_routes[i].info.min_zoom, s_routes[i].info.max_zoom);
        } else {
            uint32_t distance_tenths_km = (s_routes[i].info.distance_m + 50) / 100;
            char distance[24];
            snprintf(distance, sizeof(distance), "%lu.%lu km",
                     (unsigned long)(distance_tenths_km / 10),
                     (unsigned long)(distance_tenths_km % 10));
            lv_label_set_text_fmt(meta, "%s " LV_SYMBOL_BULLET " %s%s "
                                  LV_SYMBOL_BULLET " z%u-%u", distance,
                                  s_routes[i].info.kind == MAP_ROUTE_KIND_EXERCISE
                                      ? "exercise" : "drive",
                                  s_routes[i].info.loop ? " loop" : "",
                                  s_routes[i].info.min_zoom,
                                  s_routes[i].info.max_zoom);
        }
    }
}

static void map_refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_map) return;
    map_tile_cache_handle_t cache = map_view_get_tile_cache(s_map);
    if (!cache) return;
    uint32_t version = map_tile_cache_get_version(cache);
    if (version != s_map_cache_version) {
        s_map_cache_version = version;
        lv_obj_invalidate(s_map);
    }
}

static void nav_change_route_cb(void *ctx)
{
    (void)ctx;
    route_picker_back_cb(NULL);
}

static void gps_navigation_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_map) return;

    board_gps_status_t gps;
    if (!board_gps_get_status(&gps)) return;
    nav_fix_t fix = {
        .lat = gps.latitude,
        .lon = gps.longitude,
        .speed_mps = gps.speed_kmh / 3.6,
        .heading_deg = gps.course_deg,
        .valid = gps.fix_valid,
        .satellites = gps.satellites,
    };
    bool is_new_fix = gps.fix_valid && gps.last_fix_ms != 0 &&
                      gps.last_fix_ms != s_last_gps_fix_ms;
    if (is_new_fix) s_last_gps_fix_ms = gps.last_fix_ms;
    if (is_new_fix || !s_gps_status_seen || gps.fix_valid != s_last_fix_valid) {
        nav_engine_update(&fix, &s_nav_state);
        s_have_nav_state = s_nav_state.phase != NAV_PHASE_ACQUIRING &&
                           s_nav_state.phase != NAV_PHASE_NO_ROUTE;
        s_gps_status_seen = true;
        s_last_fix_valid = gps.fix_valid;
        nav_ui_set_fix_valid(gps.fix_valid);
        nav_ui_update(&s_nav_state);

        if (is_new_fix && s_have_nav_state &&
            (!s_have_logged_phase || s_nav_state.phase != s_last_logged_phase ||
             s_nav_state.cue != NAV_CUE_NONE)) {
            char remaining[24], turn[24];
            map_geo_format_distance(s_nav_state.remaining_m, false,
                                    remaining, sizeof(remaining));
            map_geo_format_distance(s_nav_state.dist_to_maneuver_m, false,
                                    turn, sizeof(turn));
            ESP_LOGI(TAG, "navigation %s: %s remaining, next %s in %s, %.1f km/h",
                     nav_phase_name(s_nav_state.phase), remaining,
                     s_nav_state.has_maneuver
                         ? map_maneuver_type_name(s_nav_state.maneuver_type)
                         : "none",
                     turn, s_nav_state.speed_mps * 3.6);
            s_last_logged_phase = s_nav_state.phase;
            s_have_logged_phase = true;
        }
    }
}

static bool build_static_map(lv_obj_t *screen, const char *route_path)
{
    map_route_info_t info;
    if (!map_route_peek(route_path, &info)) {
        ESP_LOGE(TAG, "route metadata unavailable: %s", route_path);
        return false;
    }

    map_route_handle_t route = map_route_load(route_path, true);
    if (!route) {
        ESP_LOGE(TAG, "route load failed: %s", route_path);
        return false;
    }

    const char *folder = map_route_get_tile_folder(route);
    if (!folder || folder[0] == '\0') folder = "tiles1";
    int min_zoom = info.min_zoom;
    int max_zoom = info.max_zoom;
    if (map_tile_cache_probe_zooms(SD_ROOT, folder, &min_zoom, &max_zoom) <= 0) {
        map_route_free(route);
        ESP_LOGE(TAG, "no usable map zooms: %s/%s", SD_ROOT, folder);
        return false;
    }
    if (!navigation_map_start(screen, folder, min_zoom, max_zoom,
                              route, false, false,
                              0.0, 0.0, 0.0, 0.0)) {
        navigation_session_cleanup(true);
        return false;
    }
    ESP_LOGI(TAG, "GPS route navigation ready: %s, %.2f km, %u points",
             info.name, info.distance_m / 1000.0, (unsigned)info.point_count);
    return true;
}

static void lvgl_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t wait_ms = lv_timer_handler();
        if (wait_ms < 5) wait_ms = 5;
        if (wait_ms > 20) wait_ms = 20;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t board_lvgl_start(bool storage_ready)
{
    lv_init();
    lv_tick_set_cb(lvgl_tick_ms);

    s_draw_buffer = heap_caps_malloc(BOARD_LCD_WIDTH * LVGL_BUFFER_LINES * sizeof(uint16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_draw_buffer) return ESP_ERR_NO_MEM;

    s_display = lv_display_create(BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT);
    if (!s_display) return ESP_ERR_NO_MEM;
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, display_flush_cb);
    lv_display_set_buffers(s_display, s_draw_buffer, NULL,
                           BOARD_LCD_WIDTH * LVGL_BUFFER_LINES * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_screen = lv_screen_active();
    bool map_ready = false;
    if (storage_ready) {
        s_route_count = 0;
        s_entry_count = 0;
        scan_routes();
        s_entry_count = s_route_count;
        scan_map_folders();
        if (s_entry_count == 1 && !s_routes[0].map_only) {
            map_ready = build_static_map(s_screen, s_routes[0].path);
        } else if (s_entry_count == 1 && s_routes[0].map_only) {
            map_ready = build_map_only(s_screen, &s_routes[0]);
        } else if (s_entry_count > 1) {
            build_route_picker(s_screen);
            map_ready = true;
        }
    }
    lv_indev_t *input = lv_indev_create();
    if (!input) return ESP_ERR_NO_MEM;
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, touch_read_cb);
    lv_indev_set_display(input, s_display);

    if (!map_ready) {
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x101820), 0);
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        s_status_label = lv_label_create(s_screen);
        lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
        lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(s_status_label,
                          storage_ready ? "No routes or map folders found"
                                        : "SD card unavailable\nCheck the card");
        lv_obj_center(s_status_label);
    }

    TaskHandle_t touch_handle = NULL;
    if (xTaskCreate(touch_task, "board_touch", 3072, NULL, 3, &touch_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(lvgl_task, "board_lvgl", 6144, NULL, 4, NULL) != pdPASS) {
        vTaskDelete(touch_handle);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
