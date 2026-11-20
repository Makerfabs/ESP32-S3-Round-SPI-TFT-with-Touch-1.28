/**
 * @file nav_engine.h
 * @brief Turns a stream of position fixes into what the driver needs to see.
 *
 * Everything stateful about guidance lives here: snapping to the route,
 * deciding when you have genuinely left it rather than just wobbled, working
 * out the next turn, and firing each announcement exactly once.
 *
 * The engine holds no UI and no LVGL. Call ::nav_engine_update from the LVGL
 * thread with the latest fix and render the ::nav_state_t it fills in.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "map_route.h"
#include "gps_source.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NAV_PHASE_NO_ROUTE = 0,  /**< route.bin missing or unreadable */
    NAV_PHASE_ACQUIRING,     /**< Waiting for the first valid fix */
    NAV_PHASE_NAVIGATING,
    NAV_PHASE_OFF_ROUTE,
    NAV_PHASE_ARRIVED,
    NAV_PHASE_FREE,          /**< A map with no route: position and the outing so far */
} nav_phase_t;

/** @brief One-shot cue emitted when a threshold is first crossed. */
typedef enum {
    NAV_CUE_NONE = 0,
    NAV_CUE_MANEUVER_FAR,    /**< ~400 m out */
    NAV_CUE_MANEUVER_NEAR,   /**< ~150 m out */
    NAV_CUE_MANEUVER_NOW,    /**< at the turn */
    NAV_CUE_OFF_ROUTE,       /**< Just left the route */
    NAV_CUE_OFF_ROUTE_FAR,   /**< Still off, and now a long way off */
    NAV_CUE_OFF_ROUTE_AGAIN, /**< Still off after NAV_OFF_ROUTE_REPEAT_S */
    NAV_CUE_BACK_ON_ROUTE,
    NAV_CUE_ARRIVED,
    NAV_CUE_LAP,             /**< Came round the start line of a closed loop */
} nav_cue_t;

typedef struct {
    nav_phase_t phase;

    /* What kind of route this is, read from route.bin. A drive counts down to
     * an arrival; a workout counts up and laps. */
    map_route_kind_t kind;
    bool             is_loop;

    /* Position as reported */
    double  lat;
    double  lon;
    double  heading_deg;
    double  speed_mps;      /**< Smoothed */
    uint8_t satellites;

    /* Snapped to the route */
    bool     on_route;
    double   cross_track_m;

    /*
     * Getting back. Valid whenever on_route is false.
     *
     * return_rel_deg is the bearing to the road measured from where the rider
     * is pointing, which is the only form a person can act on without first
 * working out which way the GPS course points.
     */
    double   return_lat;
    double   return_lon;
    double   return_rel_deg;        /**< -180..180, relative to current heading */
    const char *return_hint;        /**< "ahead", "to your left", ... never NULL */
    uint32_t off_route_s;           /**< How long the rider has been off */
    int      off_route_trend;       /**< -1 closing, 0 holding, +1 drifting away */
    uint32_t traveled_m;        /**< Along the current lap */
    uint32_t remaining_m;       /**< Left in the current lap */
    uint32_t eta_s;
    bool     eta_valid;         /**< False until a usable moving speed is available */

    /* Workout figures. Meaningful in both modes, shown in exercise mode and
     * with no route, where session_m is the ground covered. */
    uint32_t lap;               /**< Laps completed */
    uint32_t elapsed_s;         /**< Since the first valid fix */
    uint32_t session_m;         /**< Covered in total, across every lap */

    /* Next turn */
    bool                has_maneuver;
    map_maneuver_type_t maneuver_type;
    uint32_t            dist_to_maneuver_m;
    const char         *maneuver_street;      /**< Owned by the route; never NULL */

    /**
     * Cue raised by this update, or ::NAV_CUE_NONE.
     *
     * Each cue fires once. Wire this to a chime or a voice clip; the example
     * only flashes the banner.
     */
    nav_cue_t cue;
} nav_state_t;

/**
 * @brief Bind the engine to a route.
 *
 * @param route May be NULL, which puts the engine in ::NAV_PHASE_NO_ROUTE.
 */
void nav_engine_init(map_route_handle_t route);

/**
 * @brief Run with no route, for a map opened on its own.
 *
 * Fixes still come out as a position, a heading that holds still when the
 * rider does, and a smoothed speed, with the distance covered and the time
 * since the first fix - everything that does not need a road to follow. The
 * phase is ::NAV_PHASE_FREE from the first valid fix, ::NAV_PHASE_ACQUIRING
 * until then, and no cue is ever raised.
 */
void nav_engine_init_free(void);

/**
 * @brief Fold a new fix into the guidance state.
 *
 * Safe to call with an invalid fix; the phase stays ::NAV_PHASE_ACQUIRING.
 */
void nav_engine_update(const nav_fix_t *fix, nav_state_t *out);

/** @brief Human-readable phase, for logs and the status chip. */
const char *nav_phase_name(nav_phase_t phase);

#ifdef __cplusplus
}
#endif
