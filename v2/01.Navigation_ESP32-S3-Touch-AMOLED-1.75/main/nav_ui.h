/**
 * @file nav_ui.h
 * @brief Navigation screen for the 466x466 round AMOLED.
 *
 * Every control is kept inside the inscribed circle, so nothing is clipped by
 * the bezel: the turn banner and the summary panel sit where the chord is still
 * wide, and the round buttons hug the horizontal centre line where the glass is
 * widest.
 *
 * The buttons are not on screen permanently - at this size they would cover
 * the map. A tap on the map brings them up and they drop
 * away again; see NAV_CONTROLS_AUTOHIDE_MS.
 */

#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "map_route.h"
#include "nav_engine.h"
#include "nav_maps.h"
#include "compass_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the screen.
 *
 * @param route May be NULL; the UI then shows why there is nothing to follow.
 * @return false if the map view could not be created (no SD card, no memory).
 */
bool nav_ui_create(lv_obj_t *parent, map_route_handle_t route);

/**
 * @brief Build the screen for a map on its own: no route, nothing to follow.
 *
 * The same screen without the turn banner or a line to follow. The summary
 * shows what an outing without a route still has - distance covered, time and
 * speed - once there is a fix.
 *
 * It opens on @p extent, where the tiles are. The first fix that lands inside
 * it takes over, and the map follows from then on. A fix somewhere else - at
 * home, with the tiles for a trip on the card - leaves the map where the tiles
 * are, with the re-centre button up to go to the fix when asked. A drag before
 * then is taken as the rider looking somewhere on purpose, and the fix does
 * not take over at all.
 *
 * Feed it the states of an engine started with ::nav_engine_init_free.
 *
 * @param folder Tile folder at the card root
 * @param extent What the folder covers, or NULL to follow the first fix
 *               wherever it is
 */
bool nav_ui_create_map_only(lv_obj_t *parent, const char *folder,
                            const nav_map_extent_t *extent);

/** @brief Render a guidance state. Call from the LVGL thread. */
void nav_ui_update(const nav_state_t *state);

/**
 * @brief Turn the compass dial. Call from the LVGL thread.
 *
 * The needle points at true north relative to the top of the screen. Tapping
 * the dial swaps it for the heading in degrees for a few seconds, and under it
 * the direction's name - or, while the dial is faded, how far off the heading
 * may be.
 *
 * @param compass NULL, or one that is not valid, hides the dial: no sensor, or
 *                no GPS course has set north yet.
 */
void nav_ui_update_compass(const nav_compass_t *compass);

/** @brief The map widget, for prefetching or debugging. */
lv_obj_t *nav_ui_get_map(void);

/**
 * @brief Called when the rider taps the routes button.
 *
 * The UI does not switch route itself: the app owns the route handle, the
 * position source and the screens, so it does the swap.
 */
typedef void (*nav_ui_change_route_cb_t)(void *ctx);
void nav_ui_set_change_route_cb(nav_ui_change_route_cb_t cb, void *ctx);

/**
 * @brief Drop the timers this screen owns.
 *
 * Deleting the screen takes the widgets with it, but the banner-flash,
 * control-autohide and compass-readout timers are not children of anything, so
 * they would outlive it and fire on freed objects. Call this before deleting
 * the screen.
 */
void nav_ui_destroy(void);

#ifdef __cplusplus
}
#endif
