/**
 * @file nav_theme.h
 * @brief Colours and round-display geometry shared by the screens.
 *
 * Both the navigation screen and the route picker draw panels on the same round
 * 466 px panel, so the arithmetic that keeps them clear of the bezel and the
 * palette that makes them look like one product live here rather than in either
 * of them.
 *
 * The panel is a circle of radius 233. The usable chord at a vertical distance
 * d from the centre is 2*sqrt(233^2 - d^2), which falls away fast near the top
 * and bottom: 401 px at 119 from the centre, 293 at 181, 189 at 213. Every
 * width below was sized against ::nav_chord_half at the panel edge nearest the
 * rim - the edge that gets clipped first.
 */

#pragma once

#include <math.h>
#include "lvgl.h"

#define NAV_SCREEN_SIZE     466
#define NAV_SCREEN_R        (NAV_SCREEN_SIZE / 2)   /**< 233 */

/**
 * @brief Half the width available at @p dy pixels above or below the centre.
 *
 * Use it when placing something new, rather than guessing: a panel whose half
 * width exceeds this at either of its horizontal edges has its corners cut off
 * by the bezel.
 */
static inline int32_t nav_chord_half(int32_t dy)
{
    int32_t d = dy < 0 ? -dy : dy;
    if (d >= NAV_SCREEN_R) return 0;
    return (int32_t)sqrt((double)(NAV_SCREEN_R * NAV_SCREEN_R - d * d));
}

/* Panel widths, all verified against the chord at their outermost edge. */
#define NAV_PANEL_W         284     /**< Turn banner: 293 available at its top edge, y=52 */
#define NAV_LIST_W          292     /**< Route picker: 303 available at the list foot, y=410 */

#define NAV_COL_BG          lv_color_hex(0x11161C)
#define NAV_COL_BG_OFF      lv_color_hex(0x7A1E14)
#define NAV_COL_BG_DONE     lv_color_hex(0x125B2E)
#define NAV_COL_ACCENT      lv_color_hex(0x4C9AFF)
#define NAV_COL_TEXT        lv_color_hex(0xF2F5F8)
#define NAV_COL_TEXT_DIM    lv_color_hex(0x9AA6B2)
#define NAV_COL_BORDER      lv_color_hex(0x2C353F)
#define NAV_COL_WARN        lv_color_hex(0xFF6B5B)
#define NAV_COL_GOOD        lv_color_hex(0x35D07F)
#define NAV_COL_CAUTION     lv_color_hex(0xFFB020)
#define NAV_COL_NORTH       lv_color_hex(0xFF6B5B)

/** @brief The house style for a floating panel. */
static inline void nav_style_panel(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(obj, NAV_COL_BG, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_90, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, NAV_COL_BORDER, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 20, 0);
    lv_obj_set_style_pad_all(obj, 10, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

/** @brief A label with a font and colour, since every screen makes dozens. */
static inline lv_obj_t *nav_make_label(lv_obj_t *parent, const lv_font_t *font,
                                       lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}
