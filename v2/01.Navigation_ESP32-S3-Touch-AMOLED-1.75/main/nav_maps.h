/**
 * @file nav_maps.h
 * @brief The tile folders on the card, each of which opens as a map on its own.
 *
 * A route is not the only reason to put tiles on a card. A tiles-only build
 * from the packager is a rectangle of map with nothing to follow, and every
 * route build leaves a tile folder behind that is worth looking at for itself.
 * Any directory at the card root that holds zoom levels is one of these, so
 * that is all that is looked for - no index file to keep in step with the
 * tiles.
 */

#pragma once

#include <stdbool.h>
#include "nav_routes.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The ground a tile folder covers, in degrees. */
typedef struct {
    double south;
    double west;
    double north;
    double east;
} nav_map_extent_t;

/**
 * @brief List the tile folders at the card root as map-only entries.
 *
 * Each entry has ::nav_route_entry_t.map_only set, and the folder as both its
 * name and its tile folder. Folders whose names do not fit are skipped, with a
 * line in the log.
 *
 * @return Number of entries written to @p out.
 */
int nav_maps_scan(nav_route_entry_t *out, int max_entries);

/**
 * @brief What a tile folder covers, from the tiles it holds at @p zoom.
 *
 * A map with no route to open on and no fix yet would otherwise start at 0,0,
 * in the sea off West Africa. The folder already says where its tiles are.
 * Use its lowest zoom: fewest files to list, and the widest cover.
 *
 * @return false if the folder holds no tiles at that zoom.
 */
bool nav_maps_extent(const char *folder, int zoom, nav_map_extent_t *out);

/** @brief Whether a position is inside an extent. */
static inline bool nav_map_extent_contains(const nav_map_extent_t *e, double lat, double lon)
{
    return lat >= e->south && lat <= e->north && lon >= e->west && lon <= e->east;
}

#ifdef __cplusplus
}
#endif
