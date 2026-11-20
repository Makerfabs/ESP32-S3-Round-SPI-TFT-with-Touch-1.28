#pragma once
#include <stdbool.h>
#include "lvgl.h"
#include "nav_engine.h"

typedef struct {
    double south, west, north, east;
} nav_map_extent_t;

static inline bool nav_map_extent_contains(const nav_map_extent_t *e, double lat, double lon)
{
    return lat >= e->south && lat <= e->north && lon >= e->west && lon <= e->east;
}

typedef void (*nav_ui_change_route_cb_t)(void *ctx);
bool nav_ui_create(lv_obj_t *parent, map_route_handle_t route);
bool nav_ui_create_map_only(lv_obj_t *parent, const char *folder, const nav_map_extent_t *extent);
void nav_ui_update(const nav_state_t *state);
void nav_ui_set_fix_valid(bool valid);
lv_obj_t *nav_ui_get_map(void);
void nav_ui_set_change_route_cb(nav_ui_change_route_cb_t cb, void *ctx);
/* Call before removing screen objects and freeing the route. */
void nav_ui_destroy(void);
