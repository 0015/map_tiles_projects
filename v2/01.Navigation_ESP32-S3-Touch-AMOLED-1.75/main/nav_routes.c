#include "nav_routes.h"
#include "nav_config.h"
#include "nav_theme.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "map_geo.h"

static const char *TAG = "nav_routes";

/* Four rows fit between the rim and the rim. Any more and the text would have
 * to shrink past the point where it can be read at arm's length. */
#define ROW_H           72
#define ROW_GAP         8
#define LIST_W          NAV_LIST_W
#define LIST_TOP        86
#define LIST_BOTTOM     56

static struct {
    lv_obj_t                  *screen;
    const nav_route_entry_t   *entries;
    int                        count;
    nav_routes_pick_cb_t       cb;
    void                      *ctx;
} pick;

/* -------------------------------------------------------------- scanning */

static bool has_bin_suffix(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".bin") == 0;
}

int nav_routes_scan(nav_route_entry_t *out, int max_entries)
{
    if (!out || max_entries <= 0) return 0;

    int found = 0;
    DIR *dir = opendir(NAV_ROUTES_DIR);

    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && found < max_entries) {
            if (e->d_name[0] == '.' || !has_bin_suffix(e->d_name)) continue;

            nav_route_entry_t *slot = &out[found];
            slot->map_only = false;
            snprintf(slot->path, sizeof(slot->path), "%s/%s", NAV_ROUTES_DIR, e->d_name);

            if (!map_route_peek(slot->path, &slot->info)) {
                ESP_LOGW(TAG, "Not a route file, skipping: %s", slot->path);
                continue;
            }
            /* An unnamed route still needs something to tap on. */
            if (slot->info.name[0] == '\0') {
                snprintf(slot->info.name, sizeof(slot->info.name), "%s", e->d_name);
            }
            found++;
        }
        closedir(dir);
    }

    /* Cards written before routes lived in a folder have one at the root. */
    if (found == 0) {
        nav_route_entry_t *slot = &out[0];
        slot->map_only = false;
        snprintf(slot->path, sizeof(slot->path), "%s", NAV_ROUTE_PATH);
        if (map_route_peek(slot->path, &slot->info)) {
            if (slot->info.name[0] == '\0') {
                snprintf(slot->info.name, sizeof(slot->info.name), "route.bin");
            }
            found = 1;
        }
    }

    ESP_LOGI(TAG, "%d route(s) on the card", found);
    for (int i = 0; i < found; i++) {
        ESP_LOGI(TAG, "  %s: %s, %.2f km, %s%s, tiles '%s'",
                 out[i].path, out[i].info.name, out[i].info.distance_m / 1000.0,
                 out[i].info.kind == MAP_ROUTE_KIND_EXERCISE ? "exercise" : "drive",
                 out[i].info.loop ? "+loop" : "",
                 out[i].info.tile_folder[0] ? out[i].info.tile_folder : NAV_TILE_FOLDER);
    }
    return found;
}

/* ---------------------------------------------------------------- picker */

static void row_clicked_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= pick.count) return;

    ESP_LOGI(TAG, "Chose %s", pick.entries[index].info.name);
    if (pick.cb) pick.cb(&pick.entries[index], pick.ctx);
}

static void build_row(lv_obj_t *parent, const nav_route_entry_t *entry, int index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LIST_W - 16, ROW_H);
    nav_style_panel(row);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_style_margin_bottom(row, ROW_GAP, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, NAV_COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    bool workout = entry->info.kind == MAP_ROUTE_KIND_EXERCISE;

    lv_obj_t *icon = nav_make_label(row, NAV_FONT_VALUE, NAV_COL_ACCENT,
                                    entry->map_only ? LV_SYMBOL_IMAGE
                                    : workout ? LV_SYMBOL_REFRESH : LV_SYMBOL_GPS);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *name = nav_make_label(row, NAV_FONT_STREET, NAV_COL_TEXT, entry->info.name);
    lv_obj_set_width(name, LIST_W - 16 - 16 - 36);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 36, 2);

    char sub[96];
    if (entry->map_only) {
        snprintf(sub, sizeof(sub), "map only  " LV_SYMBOL_BULLET "  z%u-%u",
                 entry->info.min_zoom, entry->info.max_zoom);
    } else {
        char dist[32];
        map_geo_format_distance(entry->info.distance_m, NAV_USE_IMPERIAL, dist, sizeof(dist));
        snprintf(sub, sizeof(sub), "%s  " LV_SYMBOL_BULLET "  %s%s  " LV_SYMBOL_BULLET "  z%u-%u",
                 dist, workout ? "exercise" : "drive",
                 entry->info.loop ? " loop" : "",
                 entry->info.min_zoom, entry->info.max_zoom);
    }

    lv_obj_t *meta = nav_make_label(row, NAV_FONT_CAPTION, NAV_COL_TEXT_DIM, sub);
    lv_obj_align(meta, LV_ALIGN_BOTTOM_LEFT, 36, -2);
}

void nav_routes_show_picker(const nav_route_entry_t *entries, int count,
                            nav_routes_pick_cb_t cb, void *ctx)
{
    pick.entries = entries;
    pick.count   = count;
    pick.cb      = cb;
    pick.ctx     = ctx;

    pick.screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(pick.screen, lv_color_black(), 0);
    lv_obj_remove_flag(pick.screen, LV_OBJ_FLAG_SCROLLABLE);

    int maps = 0;
    for (int i = 0; i < count; i++) maps += entries[i].map_only;

    lv_obj_t *title = nav_make_label(pick.screen, NAV_FONT_VALUE, NAV_COL_TEXT,
                                     maps == 0 ? "Routes"
                                     : maps == count ? "Maps" : "Routes & maps");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    char sub[48];
    snprintf(sub, sizeof(sub), "%d on the card", count);
    lv_obj_t *cap = nav_make_label(pick.screen, NAV_FONT_CAPTION, NAV_COL_TEXT_DIM, sub);
    lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 58);

    /* The list scrolls, so it can be as long as the card is full. It is inset
     * from the top and bottom of the circle so no row is cut by the bezel. */
    lv_obj_t *list = lv_obj_create(pick.screen);
    lv_obj_set_size(list, LIST_W, NAV_SCREEN_SIZE - LIST_TOP - LIST_BOTTOM);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, LIST_TOP);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    /* The rows are narrower than the list so the scrollbar has somewhere to
     * live; centre them, or the whole column sits off-centre in the circle. */
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < count; i++) {
        build_row(list, &entries[i], i);
    }

    lv_screen_load(pick.screen);
}

lv_obj_t *nav_routes_get_screen(void)
{
    return pick.screen;
}
