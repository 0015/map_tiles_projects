/**
 * @file nav_routes.h
 * @brief Finding the routes on the card, and letting the rider choose one.
 *
 * A card is not limited to one route. Drop as many as you like into
 * `/sdcard/routes/` and they are listed at start-up; each one names the tile
 * folder it belongs to, so a single card can hold a commute, a training
 * circuit, and a holiday in another region without them treading on each other.
 *
 * Tiles are the bulk of the data, so routes that cover the same ground share a
 * folder. Only a route somewhere else needs its own.
 *
 * The picker lists the tile folders too, after the routes, each as a map to
 * open on its own; nav_maps.h finds them.
 */

#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "map_route.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One thing on the card the rider can open.
 *
 * Usually a route file. With @c map_only set it is a tile folder instead,
 * opened as a map with nothing to follow: @c path is the folder, and @c info
 * holds only its name - as both name and tile_folder - and the zoom levels it
 * has.
 */
typedef struct {
    char             path[128];
    map_route_info_t info;
    bool             map_only;
} nav_route_entry_t;

/**
 * @brief List the routes on the card, newest format first.
 *
 * Looks in `NAV_ROUTES_DIR`, and falls back to the single `NAV_ROUTE_PATH` for
 * cards written before routes were a folder. Only headers are read, so a card
 * with a dozen routes still lists instantly.
 *
 * @return Number of usable routes found.
 */
int nav_routes_scan(nav_route_entry_t *out, int max_entries);

/** @brief Called with the chosen route; @p entry stays valid. */
typedef void (*nav_routes_pick_cb_t)(const nav_route_entry_t *entry, void *ctx);

/**
 * @brief Put the picker on screen.
 *
 * Creates and loads its own screen, so whatever was showing can be deleted by
 * the caller once the switch has happened.
 */
void nav_routes_show_picker(const nav_route_entry_t *entries, int count,
                            nav_routes_pick_cb_t cb, void *ctx);

/** @brief The screen the picker built, so the caller can delete it after. */
lv_obj_t *nav_routes_get_screen(void);

#ifdef __cplusplus
}
#endif
