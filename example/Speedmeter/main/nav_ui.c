#include "nav_ui.h"
#include "nav_config.h"
#include "nav_theme.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "map_view.h"
#include "map_geo.h"
#include "src/draw/lv_draw_triangle.h"

static const char *TAG = "nav_ui";

/* Original navigation UI, resized for the 240 px GC9A01 round panel. */
#define CHIP_H 18
#define CHIP_Y 10
#define BANNER_W NAV_PANEL_W
#define BANNER_H 46
#define BANNER_Y 30
#define SUMMARY_W 162
#define SUMMARY_H 42
#define SUMMARY_Y (-34)
#define SUMMARY_COLUMN_W 46
#define SUMMARY_COLUMN_STEP 52
#define BTN_SIZE 32
#define BTN_EDGE 8
#define BTN_STACK 23
#define TAP_SLOP_PX 7
#define CUE_FLASH_MS 700
#define GPS_DIAL_SIZE 40

static bool fix_valid;

static struct {
    lv_obj_t *map;

    lv_obj_t *chip;
    lv_obj_t *chip_gps;

    lv_obj_t *banner;
    lv_obj_t *banner_symbol;
    lv_obj_t *banner_distance;
    lv_obj_t *banner_street;

    lv_obj_t *summary;
    lv_obj_t *val_left,  *cap_left;
    lv_obj_t *val_mid,   *cap_mid;
    lv_obj_t *val_right, *cap_right;

    lv_obj_t *btn_zoom_in;
    lv_obj_t *btn_zoom_out;
    lv_obj_t *btn_recenter;
    lv_obj_t *btn_routes;

    nav_ui_change_route_cb_t change_route_cb;
    void                    *change_route_ctx;

    /* Controls hide themselves; see NAV_CONTROLS_AUTOHIDE_MS. */
    lv_timer_t *controls_timer;
    bool        controls_shown;
    int32_t     press_travel;   /**< Finger movement since the press started */

    lv_timer_t *flash_timer;

    /* GPS course only: fixed north-up scale, arrow shows travel direction. */
    lv_obj_t *gps_dial;
    bool have_gps_course;
    double gps_course_deg;

    /* A map on its own; see nav_ui_create_map_only(). */
    bool             map_only;
    bool             have_fix;
    bool             have_extent;
    nav_map_extent_t extent;
    bool             follow_armed;  /* the first fix on the map takes over */
} ui;

static void controls_show(void);
static void controls_hide(void);
static void sync_recenter_button(void);

/**
 * Clamp a panel to the chord available at the edge of it nearest the rim.
 *
 * The constants above were all sized by hand against the circle, and this
 * makes sure they stay that way: edit one badly and the panel narrows instead
 * of having its corners cut off by the bezel.
 *
 * @param edge_y Screen y of the panel edge closest to the top or bottom of
 *               the glass - the one that runs out of chord first.
 */
static int32_t fit_width(int32_t edge_y, int32_t wanted)
{
    int32_t available = 2 * nav_chord_half(NAV_SCREEN_R - edge_y) - 4;
    return LV_MIN(wanted, available);
}

/* ----------------------------------------------------------------- buttons */

static void zoom_in_cb(lv_event_t *e)
{
    (void)e;
    map_view_zoom_in(ui.map);
    controls_show();        /* keep them up while they are being used */
}

static void zoom_out_cb(lv_event_t *e)
{
    (void)e;
    map_view_zoom_out(ui.map);
    controls_show();
}

static void center_on_extent(void)
{
    map_view_set_center(ui.map, (ui.extent.south + ui.extent.north) / 2.0,
                                (ui.extent.west + ui.extent.east) / 2.0);
}

static void recenter_cb(lv_event_t *e)
{
    (void)e;
    if (ui.map_only && !ui.have_fix) {
        /* Nobody to follow yet: back to the tiles, and let the first fix on
         * them take over as it would have. */
        if (ui.have_extent) center_on_extent();
        ui.follow_armed = true;
    } else {
        map_view_set_follow(ui.map, true);
    }
    controls_show();
}

static void routes_cb(lv_event_t *e)
{
    (void)e;
    if (ui.change_route_cb) ui.change_route_cb(ui.change_route_ctx);
}

static lv_obj_t *make_round_button_sized(lv_obj_t *parent, const char *symbol,
                                         lv_event_cb_t cb, lv_align_t align,
                                         int32_t x, int32_t y, int32_t size,
                                         const lv_font_t *font, lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_align(btn, align, x, y);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, NAV_COL_BG, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, NAV_COL_BORDER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, NAV_COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = nav_make_label(btn, font, NAV_COL_TEXT, symbol);
    lv_obj_center(l);
    if (out_label) *out_label = l;
    return btn;
}

static lv_obj_t *make_round_button(lv_obj_t *parent, const char *symbol,
                                   lv_event_cb_t cb, lv_align_t align,
                                   int32_t x, int32_t y)
{
    return make_round_button_sized(parent, symbol, cb, align, x, y,
                                   BTN_SIZE, NAV_FONT_BUTTON, NULL);
}

/* ------------------------------------------------------- showing controls
 *
 * Left on screen permanently, the four round buttons would cover a third of
 * the map on this panel, so the default state is a clean
 * screen: banner, summary, status, and map. A tap anywhere on the map brings
 * the controls up for NAV_CONTROLS_AUTOHIDE_MS.
 *
 * Re-centring is the exception. Once a drag has released follow mode, that
 * button stays up on its own until it is used - it is the only way back, and
 * a hidden control cannot tell you it is there.
 */

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void controls_hide_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    ui.controls_timer = NULL;
    controls_hide();
}

static void controls_hide(void)
{
    ui.controls_shown = false;

    if (ui.controls_timer) {
        lv_timer_delete(ui.controls_timer);
        ui.controls_timer = NULL;
    }

    set_hidden(ui.btn_zoom_in,  true);
    set_hidden(ui.btn_zoom_out, true);
    set_hidden(ui.btn_routes,   true);
    set_hidden(ui.gps_dial, !ui.have_gps_course);
    sync_recenter_button();
}

static void controls_show(void)
{
    ui.controls_shown = true;

    set_hidden(ui.btn_zoom_in,  false);
    set_hidden(ui.btn_zoom_out, false);
    set_hidden(ui.btn_routes,   false);
    set_hidden(ui.btn_recenter, false);
    /* Share the right-side space with the zoom controls. */
    set_hidden(ui.gps_dial, true);

    if (ui.controls_timer) lv_timer_delete(ui.controls_timer);
    ui.controls_timer = lv_timer_create(controls_hide_cb, NAV_CONTROLS_AUTOHIDE_MS, NULL);
    lv_timer_set_repeat_count(ui.controls_timer, 1);
}

/**
 * Show the re-centre button exactly when it is useful.
 *
 * Derived from the state rather than from having seen a change event: the
 * button is built after the map, so it would otherwise miss the transition
 * that set the initial mode and sit hidden with no way to bring it back.
 */
static void sync_recenter_button(void)
{
    if (!ui.btn_recenter) return;
    bool useful = ui.controls_shown || !map_view_get_follow(ui.map);
    set_hidden(ui.btn_recenter, !useful);
}

/** The map raises VALUE_CHANGED whenever follow mode changes. */
static void map_follow_changed_cb(lv_event_t *e)
{
    (void)e;
    sync_recenter_button();
}

/**
 * A tap on the map - as opposed to a drag of it - toggles the controls.
 *
 * LVGL sends CLICKED after any press that started and ended on the object,
 * including one that panned the map 200 px on the way, so the movement is
 * measured here rather than trusting the event. Like map_view, it is summed
 * along the path rather than taken from end to end - a drag out and back
 * finishes where it started and is still a drag - but with a looser threshold,
 * because a finger that rolls a few pixels on the glass meant to tap.
 */
static void map_input_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:
            ui.press_travel = 0;
            break;

        case LV_EVENT_PRESSING: {
            lv_point_t v;
            lv_indev_get_vect(indev, &v);
            ui.press_travel += LV_ABS(v.x) + LV_ABS(v.y);
            break;
        }

        case LV_EVENT_RELEASED:
            if (ui.press_travel > TAP_SLOP_PX) {
                /* A pan, not a tap. On a map with no route the rider is now
                 * looking somewhere on purpose, so the first fix must not
                 * drag the map away from it. */
                ui.follow_armed = false;
                break;
            }
            if (ui.controls_shown) controls_hide();
            else                   controls_show();
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------ layout */

/** Original status pill with its GPS readout; this board has no PMU. */
static void build_chip(lv_obj_t *parent)
{
    ui.chip = lv_obj_create(parent);
    nav_style_panel(ui.chip);
    lv_obj_set_size(ui.chip, LV_SIZE_CONTENT, CHIP_H);
    lv_obj_align(ui.chip, LV_ALIGN_TOP_MID, 0, CHIP_Y);
    lv_obj_set_style_radius(ui.chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(ui.chip, 6, 0);
    lv_obj_set_style_pad_ver(ui.chip, 0, 0);
    lv_obj_set_style_pad_column(ui.chip, 4, 0);
    lv_obj_set_flex_flow(ui.chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui.chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    ui.chip_gps     = nav_make_label(ui.chip, NAV_FONT_CHIP, NAV_COL_TEXT_DIM, "");
}

static void build_banner(lv_obj_t *parent)
{
    const int32_t w = fit_width(BANNER_Y, BANNER_W);

    ui.banner = lv_obj_create(parent);
    lv_obj_set_size(ui.banner, w, BANNER_H);
    lv_obj_align(ui.banner, LV_ALIGN_TOP_MID, 0, BANNER_Y);
    nav_style_panel(ui.banner);

    ui.banner_symbol = nav_make_label(ui.banner, NAV_FONT_TURN_SYMBOL, NAV_COL_ACCENT,
                                      LV_SYMBOL_GPS);
    lv_obj_align(ui.banner_symbol, LV_ALIGN_TOP_LEFT, 2, 0);

    ui.banner_distance = nav_make_label(ui.banner, NAV_FONT_TURN_DISTANCE, NAV_COL_TEXT, "");
    lv_obj_align(ui.banner_distance, LV_ALIGN_TOP_LEFT, 24, 0);

    ui.banner_street = nav_make_label(ui.banner, NAV_FONT_STREET, NAV_COL_TEXT_DIM, "");
    lv_obj_set_width(ui.banner_street, w - 12);
    lv_label_set_long_mode(ui.banner_street, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(ui.banner_street, LV_ALIGN_BOTTOM_LEFT, 2, 0);
}

/**
 * One "value over caption" column of the summary panel.
 *
 * The caption is handed back too: a drive and a workout want different figures
 * in the same three slots, so the headings are retitled at runtime. It also
 * carries the unit - at this size "45" over "km/h" reads instantly where
 * "45 km/h" on one line does not fit a third of the panel.
 */
static void build_summary_column(lv_obj_t *parent, int32_t x,
                                 lv_obj_t **out_value, lv_obj_t **out_caption)
{
    *out_value = nav_make_label(parent, NAV_FONT_VALUE, NAV_COL_TEXT, "--");
    lv_obj_set_width(*out_value, SUMMARY_COLUMN_W);
    lv_obj_set_height(*out_value, (NAV_FONT_VALUE)->line_height);
    lv_obj_set_style_text_align(*out_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(*out_value, LV_LABEL_LONG_DOT);
    lv_obj_align(*out_value, LV_ALIGN_TOP_LEFT, x, 0);

    *out_caption = nav_make_label(parent, NAV_FONT_CAPTION, NAV_COL_TEXT_DIM, "");
    lv_obj_set_width(*out_caption, SUMMARY_COLUMN_W);
    lv_obj_set_height(*out_caption, (NAV_FONT_CAPTION)->line_height);
    lv_obj_set_style_text_align(*out_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(*out_caption, LV_LABEL_LONG_DOT);
    lv_obj_align(*out_caption, LV_ALIGN_BOTTOM_LEFT, x, 0);
}

static void build_summary(lv_obj_t *parent)
{
    ui.summary = lv_obj_create(parent);
    lv_obj_set_size(ui.summary, fit_width(NAV_SCREEN_SIZE + SUMMARY_Y, SUMMARY_W), SUMMARY_H);
    lv_obj_align(ui.summary, LV_ALIGN_BOTTOM_MID, 0, SUMMARY_Y);
    nav_style_panel(ui.summary);
    lv_obj_set_style_pad_hor(ui.summary, 5, 0);
    lv_obj_set_style_pad_ver(ui.summary, 6, 0);
    build_summary_column(ui.summary, 0, &ui.val_left, &ui.cap_left);
    build_summary_column(ui.summary, SUMMARY_COLUMN_STEP, &ui.val_mid, &ui.cap_mid);
    build_summary_column(ui.summary, SUMMARY_COLUMN_STEP * 2, &ui.val_right, &ui.cap_right);
    for (int i = 1; i <= 2; ++i) {
        lv_obj_t *divider = lv_obj_create(ui.summary);
        lv_obj_remove_style_all(divider);
        lv_obj_set_size(divider, 1, 22);
        lv_obj_align(divider, LV_ALIGN_LEFT_MID,
                     SUMMARY_COLUMN_STEP * i - 3, 0);
        lv_obj_set_style_bg_color(divider, NAV_COL_BORDER, 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
        lv_obj_remove_flag(divider, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
}

/* North remains at the top, as it does on the map. This is a course display,
 * not a measurement of which way the stationary device is facing. */
static void gps_dial_draw_cb(lv_event_t *event)
{
    lv_area_t area;
    lv_obj_get_coords(ui.gps_dial, &area);
    double cx = (area.x1 + area.x2) / 2.0;
    double cy = (area.y1 + area.y2) / 2.0 + 3.0;
    double angle = ui.gps_course_deg * 3.14159265358979323846 / 180.0;
    double dx = sin(angle), dy = -cos(angle);
    lv_draw_triangle_dsc_t needle;
    lv_draw_triangle_dsc_init(&needle);
    needle.opa = fix_valid ? LV_OPA_COVER : LV_OPA_50;
    needle.p[0] = (lv_point_precise_t){cx - dy * 3.0, cy + dx * 3.0};
    needle.p[1] = (lv_point_precise_t){cx + dy * 3.0, cy - dx * 3.0};
    needle.p[2] = (lv_point_precise_t){cx + dx * 9.0, cy + dy * 9.0};
    needle.color = lv_color_hex(0xE85364);
    lv_draw_triangle(lv_event_get_layer(event), &needle);
    needle.p[2] = (lv_point_precise_t){cx - dx * 9.0, cy - dy * 9.0};
    needle.color = lv_color_hex(0xE6EDF3);
    lv_draw_triangle(lv_event_get_layer(event), &needle);
}

static void build_gps_dial(lv_obj_t *parent)
{
    ui.gps_dial = lv_obj_create(parent);
    lv_obj_set_size(ui.gps_dial, GPS_DIAL_SIZE, GPS_DIAL_SIZE);
    lv_obj_align(ui.gps_dial, LV_ALIGN_CENTER, 82, 22);
    nav_style_panel(ui.gps_dial);
    lv_obj_set_style_radius(ui.gps_dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(ui.gps_dial, 0, 0);
    lv_obj_set_style_bg_color(ui.gps_dial, lv_color_hex(0x081820), 0);
    lv_obj_set_style_bg_opa(ui.gps_dial, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ui.gps_dial, 0, 0);
    lv_obj_add_event_cb(ui.gps_dial, gps_dial_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    lv_obj_t *north = nav_make_label(ui.gps_dial, &lv_font_montserrat_10,
                                     lv_color_hex(0xE85364), "N");
    lv_obj_align(north, LV_ALIGN_TOP_MID, 0, 2);
    set_hidden(ui.gps_dial, true);
}

static void update_gps_dial(const nav_state_t *st)
{
    if (!ui.gps_dial) return;
    /* Never use the initial route-bearing fallback as a GPS course. Once a
     * moving fix has supplied a heading, hold it while slow or stopped. */
    if (fix_valid && st->speed_mps >= NAV_HEADING_MIN_SPEED_MPS &&
        isfinite(st->heading_deg)) {
        ui.gps_course_deg = st->heading_deg;
        ui.have_gps_course = true;
    }
    set_hidden(ui.gps_dial, !ui.have_gps_course || ui.controls_shown);
    if (!ui.have_gps_course) return;

    lv_obj_invalidate(ui.gps_dial);
}

/**
 * What both screens share: the map on @p folder, clamped to the zoom levels
 * the card holds, and every panel and button over it. A route, when there is
 * one, is drawn and the map opens at its start.
 */
static bool build_screen(lv_obj_t *parent, map_route_handle_t route,
                         const char *folder, bool map_only)
{
    memset(&ui, 0, sizeof(ui));
    ui.map_only = map_only;
    fix_valid = false;

    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);

    ui.map = map_view_create(parent);
    if (!ui.map) {
        ESP_LOGE(TAG, "Cannot create the map view");
        return false;
    }

    lv_obj_set_size(ui.map, NAV_SCREEN_SIZE, NAV_SCREEN_SIZE);
    lv_obj_align(ui.map, LV_ALIGN_CENTER, 0, 0);

    /* map_view measures its content area, which is still empty until LVGL runs
     * the layout. Force it now rather than at the next frame. */
    lv_obj_update_layout(ui.map);

    /* The route and marker are drawn in pixels, and this panel has few of
     * them. Left at map_view's defaults the road becomes a stripe wide enough
     * to hide the junction it is going through. */
    map_view_style_t style;
    map_view_get_style(ui.map, &style);
    style.route_width   = 4;
    style.casing_width  = 6;
    style.marker_radius = 6;
    map_view_set_style(ui.map, &style);

    if (!map_view_set_tile_source(ui.map, NAV_SD_MOUNT_POINT, folder,
                                  NAV_TILE_CACHE_SLOTS, true)) {
        ESP_LOGE(TAG, "Cannot open the tile cache at %s/%s",
                 NAV_SD_MOUNT_POINT, folder);
        return false;
    }
    ESP_LOGI(TAG, "Tiles from %s/%s", NAV_SD_MOUNT_POINT, folder);

    /* Clamp the view to the zoom levels the card actually holds. A corridor
     * download covers a few levels and nothing else; asking for one outside
     * that range is not an error anywhere in the stack, it just draws
     * placeholders, which reads as a broken map rather than a missing
     * download. */
    int card_min = 0, card_max = 0;
    int levels = map_tile_cache_probe_zooms(NAV_SD_MOUNT_POINT, folder,
                                            &card_min, &card_max);
    int zmin = NAV_ZOOM_MIN;
    int zmax = NAV_ZOOM_MAX;
    if (levels > 0) {
        zmin = LV_MAX(NAV_ZOOM_MIN, card_min);
        zmax = LV_MIN(NAV_ZOOM_MAX, card_max);
        if (zmin > zmax) {
            /* The card and nav_config.h do not overlap at all. Trust the card. */
            ESP_LOGW(TAG, "nav_config.h asks for z%d-%d but the card has z%d-%d; "
                          "using the card's range",
                     NAV_ZOOM_MIN, NAV_ZOOM_MAX, card_min, card_max);
            zmin = card_min;
            zmax = card_max;
        }
    } else {
        ESP_LOGE(TAG, "No tiles under %s/%s - the map will be blank",
                 NAV_SD_MOUNT_POINT, folder);
    }

    int zstart = NAV_ZOOM_DEFAULT;
    if (zstart < zmin) zstart = zmin;
    if (zstart > zmax) zstart = zmax;
    ESP_LOGI(TAG, "Map zoom range z%d-%d, opening at z%d", zmin, zmax, zstart);

    map_view_set_zoom_range(ui.map, zmin, zmax);
    map_view_set_zoom(ui.map, zstart);
    map_view_set_follow_offset(ui.map, NAV_FOLLOW_OFFSET_PX);
    lv_obj_add_event_cb(ui.map, map_follow_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ui.map, map_input_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(ui.map, map_input_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(ui.map, map_input_cb, LV_EVENT_RELEASED, NULL);

    if (route) {
        map_view_set_route(ui.map, route);

        /* Open where the drive begins. Framing the whole route instead would
         * be a nicer first impression, but map_view_fit_bounds() would pick a
         * zoom several levels below anything a corridor download contains -
         * a 12 km route lands on z12 - and the driver would be looking at an
         * empty grid. */
        double start_lat, start_lon;
        if (map_route_get_start(route, &start_lat, &start_lon)) {
            map_view_set_center(ui.map, start_lat, start_lon);
            map_view_set_position(ui.map, start_lat, start_lon, 0, false);
        }
    }

    build_chip(parent);
    build_banner(parent);
    build_summary(parent);

    ui.btn_zoom_in  = make_round_button(parent, LV_SYMBOL_PLUS,  zoom_in_cb,
                                        LV_ALIGN_RIGHT_MID, -BTN_EDGE, -BTN_STACK);
    ui.btn_zoom_out = make_round_button(parent, LV_SYMBOL_MINUS, zoom_out_cb,
                                        LV_ALIGN_RIGHT_MID, -BTN_EDGE,  BTN_STACK);
    ui.btn_routes   = make_round_button(parent, LV_SYMBOL_LIST,  routes_cb,
                                        LV_ALIGN_LEFT_MID,   BTN_EDGE, -BTN_STACK);
    ui.btn_recenter = make_round_button(parent, LV_SYMBOL_GPS,   recenter_cb,
                                        LV_ALIGN_LEFT_MID,   BTN_EDGE,  BTN_STACK);
    build_gps_dial(parent);

    /* Follow from the first frame. Everything below the map is now built, so
     * the VALUE_CHANGED this raises reaches a button that exists. */
    map_view_set_follow(ui.map, true);

    /* Up at start-up, so the controls are discoverable; they drop away on
     * their own a few seconds later. */
    controls_show();
    return true;
}

bool nav_ui_create(lv_obj_t *parent, map_route_handle_t route)
{
    /* A route can name the folder its tiles are in, so one card can carry
     * routes in different regions. Empty means "use the configured default". */
    const char *folder = route ? map_route_get_tile_folder(route) : "";
    if (!folder || folder[0] == '\0') folder = NAV_TILE_FOLDER;

    if (!build_screen(parent, route, folder, false)) return false;

    ESP_LOGI(TAG, "Navigation UI ready (%dx%d round), following at offset %d px",
             NAV_SCREEN_SIZE, NAV_SCREEN_SIZE, NAV_FOLLOW_OFFSET_PX);
    return true;
}

bool nav_ui_create_map_only(lv_obj_t *parent, const char *folder,
                            const nav_map_extent_t *extent)
{
    if (!folder || folder[0] == '\0') folder = NAV_TILE_FOLDER;

    if (!build_screen(parent, NULL, folder, true)) return false;

    /* Nothing to follow, so the banner has nothing to say, and the summary
     * waits for a fix to have something to count. */
    set_hidden(ui.banner, true);
    set_hidden(ui.summary, true);

    if (extent) {
        ui.extent       = *extent;
        ui.have_extent  = true;
        ui.follow_armed = true;
        center_on_extent();
        map_view_set_follow(ui.map, false);
    }

    ESP_LOGI(TAG, "Map-only UI ready on %s/%s%s", NAV_SD_MOUNT_POINT, folder,
             extent ? ", opening on its tiles" : ", following the first fix");
    return true;
}

lv_obj_t *nav_ui_get_map(void)
{
    return ui.map;
}

void nav_ui_set_change_route_cb(nav_ui_change_route_cb_t cb, void *ctx)
{
    ui.change_route_cb  = cb;
    ui.change_route_ctx = ctx;
}

void nav_ui_destroy(void)
{
    /* These timers are not children of the screen, so deleting the screen would
     * leave them running and they would fire on freed widgets. */
    if (ui.flash_timer)     { lv_timer_delete(ui.flash_timer);     ui.flash_timer = NULL; }
    if (ui.controls_timer)  { lv_timer_delete(ui.controls_timer);  ui.controls_timer = NULL; }
    memset(&ui, 0, sizeof(ui));
}

/* ------------------------------------------------------------------ update */

static void flash_end_cb(lv_timer_t *t)
{
    lv_obj_set_style_border_color(ui.banner, NAV_COL_BORDER, 0);
    lv_obj_set_style_border_width(ui.banner, 1, 0);
    lv_timer_delete(t);
    ui.flash_timer = NULL;
}

/** Briefly outline the banner so a new instruction catches the eye. */
static void flash_banner(lv_color_t color)
{
    lv_obj_set_style_border_color(ui.banner, color, 0);
    lv_obj_set_style_border_width(ui.banner, 2, 0);

    if (ui.flash_timer) lv_timer_delete(ui.flash_timer);
    ui.flash_timer = lv_timer_create(flash_end_cb, CUE_FLASH_MS, NULL);
    lv_timer_set_repeat_count(ui.flash_timer, 1);
}

/** The banner arrow points where the road is, relative to where you face. */
static const char *return_arrow_symbol(double rel_deg)
{
    double a = rel_deg < 0 ? -rel_deg : rel_deg;
    if (a <= 25.0)  return LV_SYMBOL_UP;
    if (a >= 155.0) return LV_SYMBOL_DOWN;
    return rel_deg > 0 ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT;
}

static void set_banner(lv_color_t bg, lv_color_t symbol_color,
                       const char *symbol, const char *big, const char *sub)
{
    lv_obj_set_style_bg_color(ui.banner, bg, 0);
    lv_obj_set_style_text_color(ui.banner_symbol, symbol_color, 0);
    lv_label_set_text(ui.banner_symbol, symbol);
    lv_label_set_text(ui.banner_distance, big);
    lv_label_set_text(ui.banner_street, sub);
}

/* Keep the original GPS-only status pill; no PMU or demo dependency. */

void nav_ui_set_fix_valid(bool valid)
{
    fix_valid = valid;
}

static void update_chip(const nav_state_t *st)
{
    char buf[32];
    if (!fix_valid) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " %s",
                 st->phase == NAV_PHASE_ACQUIRING ? "searching" : "lost");
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " %u", (unsigned)st->satellites);
    }
    lv_label_set_text(ui.chip_gps, buf);
}

static void set_column(lv_obj_t *value, lv_obj_t *caption,
                       const char *text, const char *heading)
{
    /* Keep the number and its unit on one line in the small round panel.
     * Choose by actual glyph width, not by the number of characters. */
    const lv_font_t *fonts[] = {
        NAV_FONT_VALUE, &lv_font_montserrat_10
    };
    const lv_font_t *font = fonts[1];
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i) {
        if (lv_text_get_width(text, (uint32_t)strlen(text), fonts[i], 0) <= SUMMARY_COLUMN_W) {
            font = fonts[i];
            break;
        }
    }
    lv_obj_set_style_text_font(value, font, 0);
    lv_obj_set_height(value, font->line_height);
    lv_label_set_text(value, text);
    lv_label_set_text(caption, heading);
}

/* Route files may contain only a Chinese name, with no English translation.
 * Use the English maneuver instruction when the built-in font cannot render
 * the supplied name. Never invent an English road name. */
static const char *english_street_or_instruction(const nav_state_t *st)
{
    if (st->has_maneuver && st->maneuver_street && st->maneuver_street[0]) {
        const unsigned char *p = (const unsigned char *)st->maneuver_street;
        bool readable = true;
        bool has_letter = false;
        for (; *p; ++p) {
            if (*p < 0x20 || *p > 0x7e) {
                readable = false;
                break;
            }
            if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
                has_letter = true;
            }
        }
        if (readable && has_letter) return st->maneuver_street;
    }
    return st->has_maneuver ? map_maneuver_type_name(st->maneuver_type)
                            : "Follow the highlighted road";
}

/**
 * Speed, split into a number and its unit.
 *
 * A third of the summary panel is 82 px, which "45 km/h" at 20 px does not
 * fit. The unit goes in the caption slot, where the column headings live
 * anyway, and the number gets the space.
 */
static void format_speed(double mps, char *buf, size_t n, const char **out_unit)
{
    snprintf(buf, n, "%u", (unsigned)(mps * 3.6 + 0.5));
    *out_unit = "km/h";
}

/* Same time value as upstream, with shorter units for the small columns. */
static void format_panel_duration(uint32_t seconds, char *buf, size_t size)
{
    if (seconds >= 3600) snprintf(buf, size, "%uh%02u", (unsigned)(seconds / 3600), (unsigned)(seconds / 60 % 60));
    else if (seconds >= 60) snprintf(buf, size, "%u min", (unsigned)((seconds + 30) / 60));
    else snprintf(buf, size, "%us", (unsigned)seconds);
}

static void update_summary(const nav_state_t *st, bool show)
{
    if (!show) {
        lv_obj_add_flag(ui.summary, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(ui.summary, LV_OBJ_FLAG_HIDDEN);

    char a[32], b[32], c[32];
    const char *speed_unit = "";
    format_speed(st->speed_mps, c, sizeof(c), &speed_unit);

    if (!st->on_route && st->phase == NAV_PHASE_OFF_ROUTE) {
        /* Distance remaining is frozen while off route, so showing it would be
         * a number that does not move. The gap to the road does move, and is
         * the one the rider is watching. */
        map_geo_format_distance(st->cross_track_m, false, a, sizeof(a));
        format_panel_duration(st->off_route_s, b, sizeof(b));
        set_column(ui.val_left, ui.cap_left, a, "off route");
        set_column(ui.val_mid,  ui.cap_mid,  b, "for");
    } else if (st->kind == MAP_ROUTE_KIND_EXERCISE || st->phase == NAV_PHASE_FREE) {
        /* A workout counts up. What matters is how far you have gone and how
         * long you have been going, not when you will be finished - and with
         * no route there is no finish to count down to anyway. */
        map_geo_format_distance(st->session_m, false, a, sizeof(a));
        format_panel_duration(st->elapsed_s, b, sizeof(b));
        set_column(ui.val_left,  ui.cap_left,  a, "distance");
        set_column(ui.val_mid,   ui.cap_mid,   b, "elapsed");
    } else {
        map_geo_format_distance(st->remaining_m, false, a, sizeof(a));
        if (st->eta_valid) format_panel_duration(st->eta_s, b, sizeof(b));
        else snprintf(b, sizeof(b), "-- min");
        set_column(ui.val_left,  ui.cap_left,  a, "distance");
        set_column(ui.val_mid,   ui.cap_mid,   b, "arrive in");
    }
    set_column(ui.val_right, ui.cap_right, c, speed_unit);
}

/**
 * A map on its own: where you are and the outing so far, and nothing about a
 * route. The banner stays hidden throughout.
 */
static void update_map_only(const nav_state_t *st)
{
    if (st->phase != NAV_PHASE_FREE) {          /* no fix yet */
        update_summary(st, false);
        return;
    }

    ui.have_fix = true;
    update_summary(st, true);
    map_view_set_position(ui.map, st->lat, st->lon, st->heading_deg, true);

    if (ui.follow_armed &&
        (!ui.have_extent || nav_map_extent_contains(&ui.extent, st->lat, st->lon))) {
        ui.follow_armed = false;
        map_view_set_follow(ui.map, true);
    }
}

void nav_ui_update(const nav_state_t *st)
{
    if (!st || !ui.map) return;

    update_chip(st);
    update_gps_dial(st);

    if (ui.map_only) {
        update_map_only(st);
        return;
    }

    switch (st->phase) {
        case NAV_PHASE_NO_ROUTE:
            set_banner(NAV_COL_BG, NAV_COL_TEXT_DIM, LV_SYMBOL_WARNING, "No route",
                       "Copy a route to the SD card");
            update_summary(st, false);
            return;

        case NAV_PHASE_ACQUIRING:
            set_banner(NAV_COL_BG, NAV_COL_ACCENT, LV_SYMBOL_GPS, "Acquiring",
                       "Waiting for a position fix");
            update_summary(st, false);
            return;

        case NAV_PHASE_ARRIVED:
            set_banner(NAV_COL_BG_DONE, NAV_COL_TEXT, LV_SYMBOL_OK, "Arrived",
                       "You have reached your destination");
            break;

        case NAV_PHASE_OFF_ROUTE: {
            /* "Off route" on its own leaves the rider to guess. Give the two
             * facts they can act on: how far the road is, and which way. */
            char dist[32], sub[96];
            map_geo_format_distance(st->cross_track_m, false, dist, sizeof(dist));

            const char *trend = (st->off_route_trend < 0) ? "  " LV_SYMBOL_OK " closing"
                              : (st->off_route_trend > 0) ? "  " LV_SYMBOL_WARNING " further"
                              : "";
            snprintf(sub, sizeof(sub), "Route is %s%s", st->return_hint, trend);

            set_banner(NAV_COL_BG_OFF, NAV_COL_TEXT,
                       return_arrow_symbol(st->return_rel_deg), dist, sub);
            break;
        }

        case NAV_PHASE_NAVIGATING:
        default: {
            /* On a loop the lap number is the thing you keep glancing at, so it
             * leads the sub-line ahead of the street name. */
            char sub[80];
            const char *street = english_street_or_instruction(st);
            if (st->is_loop) {
                snprintf(sub, sizeof(sub), "Lap %u  " LV_SYMBOL_BULLET "  %s",
                         (unsigned)(st->lap + 1), street);
            } else {
                snprintf(sub, sizeof(sub), "%s", street);
            }

            if (st->has_maneuver) {
                char dist[32];
                map_geo_format_distance(st->dist_to_maneuver_m, false,
                                        dist, sizeof(dist));
                set_banner(NAV_COL_BG, NAV_COL_ACCENT,
                           map_maneuver_type_symbol(st->maneuver_type), dist, sub);
            } else {
                set_banner(NAV_COL_BG, NAV_COL_ACCENT, LV_SYMBOL_UP, "Continue", sub);
            }
            break;
        }
    }

    update_summary(st, true);

    map_view_set_position(ui.map, st->lat, st->lon, st->heading_deg, true);
    map_view_set_progress(ui.map, st->traveled_m);
    map_view_set_off_route(ui.map, !st->on_route);
    map_view_set_return_target(ui.map, st->return_lat, st->return_lon, !st->on_route);

    switch (st->cue) {
        case NAV_CUE_MANEUVER_NOW:
        case NAV_CUE_MANEUVER_NEAR:
        case NAV_CUE_MANEUVER_FAR:
            flash_banner(NAV_COL_ACCENT);
            break;
        case NAV_CUE_OFF_ROUTE:
        case NAV_CUE_OFF_ROUTE_FAR:
        case NAV_CUE_OFF_ROUTE_AGAIN:
            flash_banner(NAV_COL_WARN);
            break;
        case NAV_CUE_ARRIVED:
        case NAV_CUE_LAP:
            flash_banner(NAV_COL_GOOD);
            break;
        default:
            break;
    }
}
