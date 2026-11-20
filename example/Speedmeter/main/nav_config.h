#pragma once

/* Navigation-engine tuning for the SR1612Z1 GPS and the existing map data. */
#define NAV_OFF_ROUTE_TOLERANCE_M  35.0
#define NAV_OFF_ROUTE_STRIKES       3
#define NAV_ON_ROUTE_STRIKES        2
#define NAV_ARRIVAL_RADIUS_M        25.0
#define NAV_OFF_ROUTE_ESCALATE_M    150.0
#define NAV_OFF_ROUTE_REPEAT_S      45
#define NAV_OFF_ROUTE_TREND_MPS     0.6
#define NAV_ANNOUNCE_FAR_M          400
#define NAV_ANNOUNCE_NEAR_M         150
#define NAV_ANNOUNCE_NOW_M          40
#define NAV_ETA_MIN_SPEED_MPS       0.5
#define NAV_ETA_SPEED_HOLD_S         60
#define NAV_FREE_MIN_SPEED_MPS      0.5
#define NAV_FREE_MAX_GAP_S          3.0
#define NAV_HEADING_MIN_SPEED_MPS   1.0
#define NAV_SPEED_SMOOTHING         0.25

/* Original navigation UI: scaled typography and map geometry for 240 px. */
#define NAV_SD_MOUNT_POINT "/sdcard"
#define NAV_TILE_FOLDER "tiles1"
#define NAV_TILE_CACHE_SLOTS 12
#define NAV_ZOOM_MIN 0
#define NAV_ZOOM_MAX 22
#define NAV_ZOOM_DEFAULT 17
#define NAV_FOLLOW_OFFSET_PX 18
#define NAV_CONTROLS_AUTOHIDE_MS 6000
#define NAV_FONT_TURN_SYMBOL &lv_font_montserrat_20
#define NAV_FONT_TURN_DISTANCE &lv_font_montserrat_20
#define NAV_FONT_STREET &lv_font_montserrat_12
#define NAV_FONT_VALUE &lv_font_montserrat_12
#define NAV_FONT_CAPTION &lv_font_montserrat_10
#define NAV_FONT_CHIP &lv_font_montserrat_12
#define NAV_FONT_BUTTON &lv_font_montserrat_16
