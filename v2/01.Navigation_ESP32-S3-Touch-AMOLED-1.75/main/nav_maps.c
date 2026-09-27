#include "nav_maps.h"
#include "nav_config.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "map_geo.h"
#include "map_tile_cache.h"

static const char *TAG = "nav_maps";

/**
 * A tile index and nothing else: digits, then @p suffix.
 *
 * Zoom and x are bare directory names, y is a file name ending in ".bin". The
 * suffix is compared without case, since a FAT card may report either.
 */
static bool parse_index(const char *name, const char *suffix, long *out)
{
    if (name[0] < '0' || name[0] > '9') return false;

    char *end = NULL;
    long v = strtol(name, &end, 10);
    if (strcasecmp(end, suffix) != 0) return false;

    *out = v;
    return true;
}

/** Does this directory hold a zoom level? Asked quietly: most of what sits at
 *  the card root is not a tile folder, and that is not news. */
static bool holds_zoom_levels(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return false;

    bool found = false;
    struct dirent *e;
    long z;
    while (!found && (e = readdir(d)) != NULL) {
        found = parse_index(e->d_name, "", &z) && z <= 22;
    }
    closedir(d);
    return found;
}

int nav_maps_scan(nav_route_entry_t *out, int max_entries)
{
    if (!out || max_entries <= 0) return 0;

    DIR *root = opendir(NAV_SD_MOUNT_POINT);
    if (!root) {
        ESP_LOGE(TAG, "Cannot list %s", NAV_SD_MOUNT_POINT);
        return 0;
    }

    int found = 0;
    struct dirent *e;
    while ((e = readdir(root)) != NULL && found < max_entries) {
        if (e->d_name[0] == '.' || e->d_type == DT_REG) continue;

        nav_route_entry_t *slot = &out[found];
        char path[sizeof(slot->path)];
        int n = snprintf(path, sizeof(path), "%s/%s", NAV_SD_MOUNT_POINT, e->d_name);
        if (n < 0 || n >= (int)sizeof(path) || !holds_zoom_levels(path)) continue;

        /* The folder travels as a route's tile_folder does, so it has to fit
         * the same field - and the tile cache's path with it. */
        if (strlen(e->d_name) >= sizeof(slot->info.tile_folder)) {
            ESP_LOGW(TAG, "Tile folder name longer than %d characters, skipping: %s",
                     (int)sizeof(slot->info.tile_folder) - 1, e->d_name);
            continue;
        }

        int lo = 0, hi = 0;
        if (map_tile_cache_probe_zooms(NAV_SD_MOUNT_POINT, e->d_name, &lo, &hi) <= 0) continue;

        memset(slot, 0, sizeof(*slot));
        slot->map_only = true;
        memcpy(slot->path, path, (size_t)n + 1);
        snprintf(slot->info.name, sizeof(slot->info.name), "%s", e->d_name);
        snprintf(slot->info.tile_folder, sizeof(slot->info.tile_folder), "%s", e->d_name);
        slot->info.min_zoom = (uint8_t)lo;
        slot->info.max_zoom = (uint8_t)hi;
        found++;
    }
    closedir(root);

    ESP_LOGI(TAG, "%d tile folder(s) on the card", found);
    return found;
}

bool nav_maps_extent(const char *folder, int zoom, nav_map_extent_t *out)
{
    if (!folder || !out) return false;

    char zpath[128];
    int n = snprintf(zpath, sizeof(zpath), "%s/%s/%d", NAV_SD_MOUNT_POINT, folder, zoom);
    if (n < 0 || n >= (int)sizeof(zpath)) return false;

    DIR *zd = opendir(zpath);
    if (!zd) return false;

    long x0 = LONG_MAX, x1 = -1, y0 = LONG_MAX, y1 = -1;
    struct dirent *xe;
    while ((xe = readdir(zd)) != NULL) {
        long x;
        if (!parse_index(xe->d_name, "", &x)) continue;

        char xpath[160];
        snprintf(xpath, sizeof(xpath), "%s/%ld", zpath, x);
        DIR *xd = opendir(xpath);
        if (!xd) continue;

        bool any = false;
        struct dirent *ye;
        while ((ye = readdir(xd)) != NULL) {
            long y;
            if (!parse_index(ye->d_name, ".bin", &y)) continue;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
            any = true;
        }
        closedir(xd);

        if (any) {
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
        }
    }
    closedir(zd);

    if (x1 < 0 || y1 < 0) return false;

    /* From the top-left corner of the first tile to the bottom-right corner of
     * the last, in world pixels and then degrees. */
    const double tile = MAP_TILE_CACHE_TILE_SIZE;
    out->west  = map_geo_world_px_to_lon(x0 * tile, zoom);
    out->east  = map_geo_world_px_to_lon((x1 + 1) * tile, zoom);
    out->north = map_geo_world_py_to_lat(y0 * tile, zoom);
    out->south = map_geo_world_py_to_lat((y1 + 1) * tile, zoom);

    ESP_LOGI(TAG, "%s covers %.5f,%.5f to %.5f,%.5f (%ld x %ld tiles at z%d)",
             folder, out->south, out->west, out->north, out->east,
             x1 - x0 + 1, y1 - y0 + 1, zoom);
    return true;
}
