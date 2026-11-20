#include "nav_engine.h"
#include "nav_config.h"

#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "map_geo.h"

static const char *TAG = "nav_engine";

/* Closing or drifting is judged over this long: long enough that a metre or
 * two of GPS noise does not flip the arrow, short enough that a rider who
 * turns back sees it change within a few seconds. */
#define TREND_WINDOW_S              3.0

/** Which announcement thresholds have already fired for the current maneuver. */
typedef enum {
    ANNOUNCED_NOTHING = 0,
    ANNOUNCED_FAR,
    ANNOUNCED_NEAR,
    ANNOUNCED_NOW,
} announce_stage_t;

static struct {
    map_route_handle_t route;
    nav_phase_t        phase;

    double smoothed_speed_mps;
    bool   have_speed;
    double eta_speed_mps;
    int64_t eta_last_moving_us;

    /* Last heading seen while genuinely moving; see NAV_HEADING_MIN_SPEED_MPS. */
    double held_heading_deg;
    bool   have_heading;

    int  off_strikes;
    int  on_strikes;
    bool off_route_latched;

    /* Off-route episode: when it began, and what the rider has been told. */
    int64_t off_since_us;
    int64_t off_last_said_us;
    bool    off_said_far;

    /* Closing or drifting, over TREND_WINDOW_S: where the window started, and
     * what the last full one said, held until the next one closes. */
    int64_t trend_since_us;
    double  trend_since_xte;
    int     trend;

    int              announced_maneuver;   /**< Index the stage below refers to */
    announce_stage_t stage;

    bool arrived_latched;

    /** Last state built from a valid fix, replayed during a GPS dropout. */
    nav_state_t last;
    bool        have_last;

    /* Workout tracking */
    map_route_kind_t kind;
    bool     is_loop;
    uint32_t laps;
    uint32_t session_base_m;    /**< Distance banked by completed laps */
    int64_t  started_us;        /**< First valid fix; 0 until then */

    /* No route at all; see nav_engine_init_free(). */
    bool    free_roam;
    double  free_m;             /**< Ground covered */
    int64_t free_last_us;       /**< When the last valid fix was taken in; 0 before one */
    double  free_last_lat;
    double  free_last_lon;
} s;

void nav_engine_init(map_route_handle_t route)
{
    memset(&s, 0, sizeof(s));
    s.route              = route;
    s.announced_maneuver = -1;
    s.phase              = route ? NAV_PHASE_ACQUIRING : NAV_PHASE_NO_ROUTE;

    if (route) {
        map_route_set_arrival_radius(route, NAV_ARRIVAL_RADIUS_M);
        s.kind    = map_route_get_kind(route);
        s.is_loop = map_route_is_loop(route);

        uint32_t dist = map_route_get_total_distance_m(route);
        uint32_t secs = map_route_get_total_duration_s(route);
        double planned_speed_mps = (secs > 0) ? (double)dist / (double)secs : 10.0;

        ESP_LOGI(TAG, "Route: %.2f km, planned %.1f km/h, %d turns, %s%s",
                 dist / 1000.0, planned_speed_mps * 3.6,
                 map_route_get_maneuver_count(route),
                 (s.kind == MAP_ROUTE_KIND_EXERCISE) ? "exercise" : "drive",
                 s.is_loop ? ", closed loop" : "");
    }
}

void nav_engine_init_free(void)
{
    memset(&s, 0, sizeof(s));
    s.free_roam          = true;
    s.announced_maneuver = -1;
    s.phase              = NAV_PHASE_ACQUIRING;
    ESP_LOGI(TAG, "No route: position, speed and distance covered only");
}

/**
 * Decide whether we are off route, with hysteresis in both directions.
 *
 * A single bad fix under a bridge should not trigger "off route", and a single
 * lucky fix should not clear it either.
 */
static bool update_off_route(bool match_on_route)
{
    if (match_on_route) {
        s.on_strikes++;
        s.off_strikes = 0;
        if (s.off_route_latched && s.on_strikes >= NAV_ON_ROUTE_STRIKES) {
            s.off_route_latched = false;
        }
    } else {
        s.off_strikes++;
        s.on_strikes = 0;
        if (!s.off_route_latched && s.off_strikes >= NAV_OFF_ROUTE_STRIKES) {
            s.off_route_latched = true;
        }
    }
    return s.off_route_latched;
}

/**
 * Where the road is, said the way a person would say it.
 *
 * Relative to where the rider is pointing, not to north: "to your left" is
 * actionable, "bearing 274" is not.
 */
static const char *return_hint_for(double rel_deg)
{
    double a = rel_deg < 0 ? -rel_deg : rel_deg;
    if (a <= 25.0)  return "straight ahead";
    if (a >= 155.0) return "behind you";
    return rel_deg > 0 ? "to your right" : "to your left";
}

/** Fire each distance threshold once per maneuver, nearest one wins. */
static nav_cue_t update_announcements(int maneuver_index, uint32_t dist_m)
{
    if (maneuver_index < 0) return NAV_CUE_NONE;

    if (maneuver_index != s.announced_maneuver) {
        s.announced_maneuver = maneuver_index;
        s.stage              = ANNOUNCED_NOTHING;
    }

    if (dist_m <= NAV_ANNOUNCE_NOW_M && s.stage < ANNOUNCED_NOW) {
        s.stage = ANNOUNCED_NOW;
        return NAV_CUE_MANEUVER_NOW;
    }
    if (dist_m <= NAV_ANNOUNCE_NEAR_M && s.stage < ANNOUNCED_NEAR) {
        s.stage = ANNOUNCED_NEAR;
        return NAV_CUE_MANEUVER_NEAR;
    }
    if (dist_m <= NAV_ANNOUNCE_FAR_M && s.stage < ANNOUNCED_FAR) {
        s.stage = ANNOUNCED_FAR;
        return NAV_CUE_MANEUVER_FAR;
    }
    return NAV_CUE_NONE;
}

/**
 * No route to measure progress along, so measure the ground covered instead:
 * the smoothed speed, added up over the time between fixes. See
 * NAV_FREE_MIN_SPEED_MPS for why not from the positions.
 */
static void update_free(const nav_fix_t *fix, nav_state_t *out)
{
    int64_t now_us = esp_timer_get_time();

    if (s.free_last_us != 0) {
        double dt = (now_us - s.free_last_us) / 1000000.0;
        if (dt > NAV_FREE_MAX_GAP_S) {
            /* Fixes stopped for a while. The straight line across the gap is
             * short of the truth on a winding road, and never wildly over it
             * the way the speed before the gap times its length could be. */
            s.free_m += map_geo_distance_m(s.free_last_lat, s.free_last_lon,
                                           fix->lat, fix->lon);
        } else if (s.smoothed_speed_mps >= NAV_FREE_MIN_SPEED_MPS) {
            s.free_m += s.smoothed_speed_mps * dt;
        }
    }
    s.free_last_us  = now_us;
    s.free_last_lat = fix->lat;
    s.free_last_lon = fix->lon;

    out->phase     = NAV_PHASE_FREE;
    out->on_route  = true;          /* nothing to be off */
    out->session_m = (uint32_t)s.free_m;
    out->elapsed_s = (uint32_t)((now_us - s.started_us) / 1000000);

    s.phase     = NAV_PHASE_FREE;
    s.last      = *out;
    s.have_last = true;
}

void nav_engine_update(const nav_fix_t *fix, nav_state_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->maneuver_street = "";
    out->return_hint     = "";

    if (!s.route && !s.free_roam) {
        out->phase = NAV_PHASE_NO_ROUTE;
        return;
    }

    if (!fix || !fix->valid) {
        /* Replay the last good state rather than a blank one. Zeroing here
         * would report position 0,0 and march the marker off to the Atlantic
         * every time the receiver blinks. */
        if (s.have_last) {
            *out = s.last;
            out->cue        = NAV_CUE_NONE;   /* cues fire once, on live data */
            out->satellites = fix ? fix->satellites : 0;
            return;
        }
        out->phase      = NAV_PHASE_ACQUIRING;
        out->satellites = fix ? fix->satellites : 0;
        return;
    }

    /* Smooth the speed; raw GPS speed is noisy at walking pace. */
    if (!s.have_speed) {
        s.smoothed_speed_mps = fix->speed_mps;
        s.have_speed = true;
    } else {
        s.smoothed_speed_mps += (fix->speed_mps - s.smoothed_speed_mps) * NAV_SPEED_SMOOTHING;
    }

    if (s.started_us == 0) s.started_us = esp_timer_get_time();

    out->kind    = s.kind;
    out->is_loop = s.is_loop;

    /* Believe the course only while there is movement to have a course in. */
    if (s.smoothed_speed_mps >= NAV_HEADING_MIN_SPEED_MPS) {
        s.held_heading_deg = fix->heading_deg;
        s.have_heading     = true;
    }

    out->lat         = fix->lat;
    out->lon         = fix->lon;
    out->heading_deg = s.have_heading ? s.held_heading_deg : 0.0;
    out->speed_mps   = s.smoothed_speed_mps;
    out->satellites  = fix->satellites;

    if (s.free_roam) {
        update_free(fix, out);
        return;
    }

    map_route_match_t m;
    if (!map_route_match(s.route, fix->lat, fix->lon, NAV_OFF_ROUTE_TOLERANCE_M, &m)) {
        out->phase = NAV_PHASE_ACQUIRING;
        return;
    }

    /* GPS course is unreliable while stopped. When a route can provide a
     * useful direction, orient the marker along it until movement resumes. */
    if (!s.have_heading && s.smoothed_speed_mps < NAV_HEADING_MIN_SPEED_MPS) {
        out->heading_deg = m.route_bearing_deg;
    }

    out->cross_track_m = m.cross_track_m;
    out->traveled_m    = m.traveled_m;
    out->remaining_m   = m.remaining_m;

    /* Bank the lap before deriving the session total, so the figure never
     * dips as the counter rolls back to the start line. */
    if (m.lapped) {
        s.laps++;
        s.session_base_m += map_route_get_total_distance_m(s.route);
        ESP_LOGI(TAG, "Lap %u complete", (unsigned)s.laps);
    }
    out->lap       = s.laps;
    out->session_m = s.session_base_m + m.traveled_m;
    out->elapsed_s = (uint32_t)((esp_timer_get_time() - s.started_us) / 1000000);

    bool off = update_off_route(m.on_route);
    out->on_route = !off;

    /* Everything the rider needs to get back. Computed whether or not the
     * hysteresis has latched yet, so the numbers are already right the instant
     * the banner appears. */
    int64_t now_us = esp_timer_get_time();
    out->return_lat         = m.return_lat;
    out->return_lon         = m.return_lon;
    out->return_rel_deg     = map_geo_angle_diff_deg(out->heading_deg, m.return_bearing_deg);
    out->return_hint        = return_hint_for(out->return_rel_deg);

    if (off) {
        if (s.off_since_us == 0) {
            s.off_since_us     = now_us;
            s.off_last_said_us = now_us;
            s.off_said_far     = false;
        }
        out->off_route_s = (uint32_t)((now_us - s.off_since_us) / 1000000);

        /* Closing or drifting, judged over a window of its own rather than
         * per fix, so a metre of GPS noise does not flip the arrow - and not
         * over the time since the last announcement, which can be most of a
         * minute and would still say "further" long after the rider turned
         * back. */
        if (s.trend_since_us == 0) {
            s.trend_since_us  = now_us;
            s.trend_since_xte = m.cross_track_m;
        } else {
            double dt = (now_us - s.trend_since_us) / 1000000.0;
            if (dt >= TREND_WINDOW_S) {
                double rate = (m.cross_track_m - s.trend_since_xte) / dt;
                s.trend = (rate < -NAV_OFF_ROUTE_TREND_MPS) ? -1
                        : (rate >  NAV_OFF_ROUTE_TREND_MPS) ?  1 : 0;
                s.trend_since_us  = now_us;
                s.trend_since_xte = m.cross_track_m;
            }
        }
        out->off_route_trend = s.trend;
    } else {
        s.off_since_us   = 0;
        s.off_said_far   = false;
        s.trend_since_us = 0;
        s.trend          = 0;
        out->off_route_s = 0;
    }

    /* Estimate from actual movement, including walking. Check raw speed too,
     * so a smoothed tail after stopping cannot replace the held moving speed.
     * A brief stop keeps that speed; a long stop needs fresh movement. */
    if (isfinite(fix->speed_mps) &&
        fix->speed_mps >= NAV_ETA_MIN_SPEED_MPS &&
        s.smoothed_speed_mps >= NAV_ETA_MIN_SPEED_MPS) {
        s.eta_speed_mps = s.smoothed_speed_mps;
        s.eta_last_moving_us = now_us;
    }
    out->eta_valid = s.eta_speed_mps >= NAV_ETA_MIN_SPEED_MPS &&
                     (now_us - s.eta_last_moving_us) <=
                         (int64_t)NAV_ETA_SPEED_HOLD_S * 1000000;
    if (out->eta_valid) {
        out->eta_s = (uint32_t)((double)m.remaining_m / s.eta_speed_mps);
    }
    if (m.arrived) {
        out->eta_valid = true;
        out->eta_s = 0;
    }

    if (m.next_maneuver >= 0) {
        map_maneuver_t mv;
        if (map_route_get_maneuver(s.route, m.next_maneuver, &mv)) {
            out->has_maneuver       = true;
            out->maneuver_type      = mv.type;
            out->dist_to_maneuver_m = m.dist_to_maneuver_m;
            out->maneuver_street    = mv.street;
        }
    }

    /* Phase and cue, most important state last so it wins. */
    nav_cue_t cue = NAV_CUE_NONE;

    if (off) {
        if (s.phase != NAV_PHASE_OFF_ROUTE) {
            cue = NAV_CUE_OFF_ROUTE;
            s.off_last_said_us = now_us;
        } else if (!s.off_said_far && m.cross_track_m >= NAV_OFF_ROUTE_ESCALATE_M) {
            /* It is getting worse, not better. Say so once. */
            cue = NAV_CUE_OFF_ROUTE_FAR;
            s.off_said_far     = true;
            s.off_last_said_us = now_us;
        } else if ((now_us - s.off_last_said_us) / 1000000 >= NAV_OFF_ROUTE_REPEAT_S) {
            /* Minutes of silence reads as the device having given up. */
            cue = NAV_CUE_OFF_ROUTE_AGAIN;
            s.off_last_said_us = now_us;
        }
        s.phase = NAV_PHASE_OFF_ROUTE;
    } else {
        if (s.phase == NAV_PHASE_OFF_ROUTE) cue = NAV_CUE_BACK_ON_ROUTE;
        s.phase = NAV_PHASE_NAVIGATING;

        nav_cue_t turn_cue = update_announcements(m.next_maneuver, m.dist_to_maneuver_m);
        if (turn_cue != NAV_CUE_NONE) cue = turn_cue;
    }

    if (m.lapped) cue = NAV_CUE_LAP;

    if (m.arrived) {
        s.phase = NAV_PHASE_ARRIVED;
        if (!s.arrived_latched) {
            s.arrived_latched = true;
            cue = NAV_CUE_ARRIVED;
        }
    } else {
        s.arrived_latched = false;
    }

    out->phase = s.phase;
    out->cue   = cue;

    s.last      = *out;
    s.have_last = true;
}

const char *nav_phase_name(nav_phase_t phase)
{
    switch (phase) {
        case NAV_PHASE_NO_ROUTE:   return "no route";
        case NAV_PHASE_ACQUIRING:  return "acquiring";
        case NAV_PHASE_NAVIGATING: return "navigating";
        case NAV_PHASE_OFF_ROUTE:  return "off route";
        case NAV_PHASE_ARRIVED:    return "arrived";
        case NAV_PHASE_FREE:       return "map only";
        default:                   return "?";
    }
}
