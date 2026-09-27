/**
 * @file gps_source.h
 * @brief One position feed, backed either by the LC76G module or by a simulator.
 *
 * The rest of the app never learns which one it is talking to, so the guidance
 * logic and the UI are exercised identically on a desk and in a car.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "map_route.h"

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
    bool     simulated;
    uint8_t  satellites;
    uint32_t timestamp_ms;
} nav_fix_t;

/**
 * @brief Called from the source's own task on every new report.
 *
 * Do not touch LVGL from here; hand the fix to the LVGL thread instead.
 */
typedef void (*nav_fix_cb_t)(const nav_fix_t *fix, void *ctx);

/**
 * @brief Start producing fixes.
 *
 * @param route Needed only by the simulator, which drives along it. May be NULL
 *              when the real receiver is selected.
 */
bool gps_source_start(map_route_handle_t route, nav_fix_cb_t cb, void *ctx);

void gps_source_stop(void);

/** @brief "LC76G" or "Simulator", for the status chip. */
const char *gps_source_name(void);

bool gps_source_is_simulated(void);

/* ----------------------------------------------------------- demo controls
 *
 * Transport controls for the simulated drive. All of these are no-ops when the
 * build is using the real receiver, so the UI can call them unconditionally.
 */

/** @brief Freeze the vehicle in place. It keeps reporting, at zero speed. */
void gps_source_sim_set_paused(bool paused);
bool gps_source_sim_is_paused(void);

/** @brief Multiply the cruising speed. 1.0 is ::NAV_SIM_SPEED_KMH. */
void  gps_source_sim_set_speed_scale(float scale);
float gps_source_sim_get_speed_scale(void);

/** @brief Jump back to the start of the route. */
void gps_source_sim_restart(void);

/**
 * @brief Jump to just before the next turn.
 *
 * Lands ::NAV_DEMO_SKIP_LEAD_M short of it, so the approach and the
 * announcements still play out. Does nothing when no turn remains.
 */
void gps_source_sim_skip_to_next_maneuver(void);

/** @brief How far along the route the simulated vehicle is, in metres. */
double gps_source_sim_get_traveled_m(void);

/**
 * @brief Pull the simulated rider off the road, or let them drift back.
 *
 * Off-route handling is the one part of guidance a demo cannot reach by
 * driving the route correctly, and the part you least want to debug at the
 * roadside. This walks the position sideways off the route so the alerts,
 * the tether and the direction hints can all be watched from a desk.
 */
void gps_source_sim_set_straying(bool straying);
bool gps_source_sim_is_straying(void);

#ifdef __cplusplus
}
#endif
