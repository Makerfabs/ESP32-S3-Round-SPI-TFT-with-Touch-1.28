/**
 * @file gps_source.h
 * @brief Normalized position sample consumed by the navigation engine.
 *
 * The current board adapter fills this structure from the SR1612Z1 NMEA
 * receiver. Keeping the engine input independent of the UART parser lets the
 * navigation logic remain separate from board hardware.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A position report, normalised to SI units. */
typedef struct {
    double   lat;
    double   lon;
    double   speed_mps;
    double   heading_deg;       /**< Course over ground, clockwise from north */
    bool     valid;             /**< false while there is no fix */
    uint8_t  satellites;
} nav_fix_t;

#ifdef __cplusplus
}
#endif
