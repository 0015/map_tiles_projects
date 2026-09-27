#include "nav_ui.h"
#include "nav_config.h"
#include "nav_theme.h"
#include "nav_power.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "map_view.h"
#include "map_geo.h"
#include "gps_source.h"

static const char *TAG = "nav_ui";

/* Round-display geometry, 466 px across, radius 233.
 *
 * Each panel width below is the chord available at its outermost edge, less a
 * few pixels of margin; nav_chord_half() in nav_theme.h is the arithmetic.
 * The vertical budget is the thing that is actually scarce here:
 *
 *   0 .. 20    rim, 189 px of chord at the bottom of it and falling fast
 *   20 .. 46   status pill
 *   52 .. 130  turn banner
 *   130 .. 352 map, and nothing else - the only band you can read a map in
 *   352 .. 420 summary
 *   420 .. 466 rim
 */
#define CHIP_H              26
#define CHIP_Y              20      /* chord at y=20 is 189, so keep it short */

#define BANNER_W            NAV_PANEL_W
#define BANNER_H            78
#define BANNER_Y            52      /* chord at y=52 is 293 */

#define SUMMARY_W           268
#define SUMMARY_H           68
#define SUMMARY_Y           (-46)   /* chord at y=420 is 278 */

/* Controls. 56 px is about 5.5 mm on this glass - the smallest target that is
 * comfortable with a thumb, and four of them still leave the map readable
 * because they are only up for a few seconds at a time. */
#define BTN_SIZE            56
#define BTN_EDGE            16
#define BTN_STACK           44

/* Demo transport bar, above the summary panel where the circle is still wide. */
#define DEMO_BAR_W          264
#define DEMO_BAR_H          56
#define DEMO_BAR_Y          (-124)
#define DEMO_BTN_SIZE       44
#define DEMO_BTN_SPACING    50

/* Finger travel below which a press counts as a tap rather than a pan. */
#define TAP_SLOP_PX         12

/* Compass dial, at half past four. Top right would be the obvious place, but
 * here that is where the zoom-in button comes up. Down here it clears
 * everything that can share the screen with it: its centre is 189 px from the
 * screen's and its edge 217, inside the 233 px circle, and it stays 10 px off
 * the zoom-out button, 9 px off the summary panel and 3 px off the demo bar.
 * 56 px across, the same thumb-sized target as the buttons, since a tap opens
 * its readout. */
#define COMPASS_SIZE        56
#define COMPASS_DX          159         /* centre, from the screen's */
#define COMPASS_DY          103
#define COMPASS_NEEDLE_LEN  12
#define COMPASS_NEEDLE_W    5
#define COMPASS_N_RADIUS    19          /* where the "N" sits, from the centre */
#define COMPASS_REDRAW_DEG  1.0f        /* smaller turns are not worth a redraw */
#define COMPASS_READOUT_MS  5000

#define CUE_FLASH_MS        700

static struct {
    lv_obj_t *map;

    lv_obj_t *chip;
    lv_obj_t *chip_gps;
    lv_obj_t *chip_battery;

    lv_obj_t *banner;
    lv_obj_t *banner_symbol;
    lv_obj_t *banner_distance;
    lv_obj_t *banner_street;

    lv_obj_t *summary;
    lv_obj_t *val_left,  *cap_left;
    lv_obj_t *val_mid,   *cap_mid;
    lv_obj_t *val_right, *cap_right;

    lv_obj_t *btn_zoom_in;
    lv_obj_t *btn_zoom_out;
    lv_obj_t *btn_recenter;
    lv_obj_t *btn_routes;

    nav_ui_change_route_cb_t change_route_cb;
    void                    *change_route_ctx;

    lv_obj_t   *demo_bar;
    lv_obj_t   *demo_play_label;
    lv_obj_t   *demo_speed_label;
    lv_obj_t   *demo_stray_btn;
    int         demo_speed_step;

    /* Controls hide themselves; see NAV_CONTROLS_AUTOHIDE_MS. */
    lv_timer_t *controls_timer;
    bool        controls_shown;
    int32_t     press_travel;   /**< Finger movement since the press started */

    lv_obj_t   *compass;
    lv_obj_t   *compass_value;      /* readout: the heading in degrees */
    lv_obj_t   *compass_caption;    /* readout: "NNW", or how far off it may be */
    lv_timer_t *compass_readout_timer;
    float       compass_deg;        /* heading the needle is drawn for */
    int         compass_err_deg;
    bool        compass_trusted;

    lv_timer_t *flash_timer;

    /* A map on its own; see nav_ui_create_map_only(). */
    bool             map_only;
    bool             have_fix;
    bool             have_extent;
    nav_map_extent_t extent;
    bool             follow_armed;  /* the first fix on the map takes over */
} ui;

static const float DEMO_SPEEDS[] = NAV_DEMO_SPEED_STEPS;
#define DEMO_SPEED_COUNT (int)(sizeof(DEMO_SPEEDS) / sizeof(DEMO_SPEEDS[0]))

static void controls_show(void);
static void controls_hide(void);
static void sync_recenter_button(void);

/**
 * Clamp a panel to the chord available at the edge of it nearest the rim.
 *
 * The constants above were all sized by hand against the circle, and this
 * makes sure they stay that way: edit one badly and the panel narrows instead
 * of having its corners cut off by the bezel.
 *
 * @param edge_y Screen y of the panel edge closest to the top or bottom of
 *               the glass - the one that runs out of chord first.
 */
static int32_t fit_width(int32_t edge_y, int32_t wanted)
{
    int32_t available = 2 * nav_chord_half(NAV_SCREEN_R - edge_y) - 8;
    return LV_MIN(wanted, available);
}

/* ----------------------------------------------------------------- buttons */

static void zoom_in_cb(lv_event_t *e)
{
    (void)e;
    map_view_zoom_in(ui.map);
    controls_show();        /* keep them up while they are being used */
}

static void zoom_out_cb(lv_event_t *e)
{
    (void)e;
    map_view_zoom_out(ui.map);
    controls_show();
}

static void center_on_extent(void)
{
    map_view_set_center(ui.map, (ui.extent.south + ui.extent.north) / 2.0,
                                (ui.extent.west + ui.extent.east) / 2.0);
}

static void recenter_cb(lv_event_t *e)
{
    (void)e;
    if (ui.map_only && !ui.have_fix) {
        /* Nobody to follow yet: back to the tiles, and let the first fix on
         * them take over as it would have. */
        if (ui.have_extent) center_on_extent();
        ui.follow_armed = true;
    } else {
        map_view_set_follow(ui.map, true);
    }
    controls_show();
}

static void routes_cb(lv_event_t *e)
{
    (void)e;
    if (ui.change_route_cb) ui.change_route_cb(ui.change_route_ctx);
}

static lv_obj_t *make_round_button_sized(lv_obj_t *parent, const char *symbol,
                                         lv_event_cb_t cb, lv_align_t align,
                                         int32_t x, int32_t y, int32_t size,
                                         const lv_font_t *font, lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_align(btn, align, x, y);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, NAV_COL_BG, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, NAV_COL_BORDER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, NAV_COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = nav_make_label(btn, font, NAV_COL_TEXT, symbol);
    lv_obj_center(l);
    if (out_label) *out_label = l;
    return btn;
}

static lv_obj_t *make_round_button(lv_obj_t *parent, const char *symbol,
                                   lv_event_cb_t cb, lv_align_t align,
                                   int32_t x, int32_t y)
{
    return make_round_button_sized(parent, symbol, cb, align, x, y,
                                   BTN_SIZE, NAV_FONT_BUTTON, NULL);
}

/* ------------------------------------------------------- showing controls
 *
 * Left on screen permanently, the four round buttons would cover a third of
 * the map on this panel, so the default state is a clean
 * screen: banner, summary, status, and map. A tap anywhere on the map brings
 * the controls up for NAV_CONTROLS_AUTOHIDE_MS.
 *
 * Re-centring is the exception. Once a drag has released follow mode, that
 * button stays up on its own until it is used - it is the only way back, and
 * a hidden control cannot tell you it is there.
 */

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void controls_hide_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    ui.controls_timer = NULL;
    controls_hide();
}

static void controls_hide(void)
{
    ui.controls_shown = false;

    if (ui.controls_timer) {
        lv_timer_delete(ui.controls_timer);
        ui.controls_timer = NULL;
    }

    set_hidden(ui.btn_zoom_in,  true);
    set_hidden(ui.btn_zoom_out, true);
    set_hidden(ui.btn_routes,   true);
    set_hidden(ui.demo_bar,     true);
    sync_recenter_button();
}

static void controls_show(void)
{
    ui.controls_shown = true;

    set_hidden(ui.btn_zoom_in,  false);
    set_hidden(ui.btn_zoom_out, false);
    set_hidden(ui.btn_routes,   false);
    set_hidden(ui.btn_recenter, false);
    set_hidden(ui.demo_bar,     false);

    if (ui.controls_timer) lv_timer_delete(ui.controls_timer);
    ui.controls_timer = lv_timer_create(controls_hide_cb, NAV_CONTROLS_AUTOHIDE_MS, NULL);
    lv_timer_set_repeat_count(ui.controls_timer, 1);
}

/**
 * Show the re-centre button exactly when it is useful.
 *
 * Derived from the state rather than from having seen a change event: the
 * button is built after the map, so it would otherwise miss the transition
 * that set the initial mode and sit hidden with no way to bring it back.
 */
static void sync_recenter_button(void)
{
    if (!ui.btn_recenter) return;
    bool useful = ui.controls_shown || !map_view_get_follow(ui.map);
    set_hidden(ui.btn_recenter, !useful);
}

/** The map raises VALUE_CHANGED whenever follow mode changes. */
static void map_follow_changed_cb(lv_event_t *e)
{
    (void)e;
    sync_recenter_button();
}

/**
 * A tap on the map - as opposed to a drag of it - toggles the controls.
 *
 * LVGL sends CLICKED after any press that started and ended on the object,
 * including one that panned the map 200 px on the way, so the movement is
 * measured here rather than trusting the event. Like map_view, it is summed
 * along the path rather than taken from end to end - a drag out and back
 * finishes where it started and is still a drag - but with a looser threshold,
 * because a finger that rolls a few pixels on the glass meant to tap.
 */
static void map_input_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:
            ui.press_travel = 0;
            break;

        case LV_EVENT_PRESSING: {
            lv_point_t v;
            lv_indev_get_vect(indev, &v);
            ui.press_travel += LV_ABS(v.x) + LV_ABS(v.y);
            break;
        }

        case LV_EVENT_RELEASED:
            if (ui.press_travel > TAP_SLOP_PX) {
                /* A pan, not a tap. On a map with no route the rider is now
                 * looking somewhere on purpose, so the first fix must not
                 * drag the map away from it. */
                ui.follow_armed = false;
                break;
            }
            if (ui.controls_shown) controls_hide();
            else                   controls_show();
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------ layout */

/**
 * Status pill: satellites on the left, battery on the right.
 *
 * Two labels rather than one string, because the battery figure has to be able
 * to turn red on its own. It sits above the banner, where the circle is only
 * 189 px across - which is why the readouts are this terse.
 */
static void build_chip(lv_obj_t *parent)
{
    ui.chip = lv_obj_create(parent);
    nav_style_panel(ui.chip);
    lv_obj_set_size(ui.chip, LV_SIZE_CONTENT, CHIP_H);
    lv_obj_align(ui.chip, LV_ALIGN_TOP_MID, 0, CHIP_Y);
    lv_obj_set_style_radius(ui.chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(ui.chip, 12, 0);
    lv_obj_set_style_pad_ver(ui.chip, 0, 0);
    lv_obj_set_style_pad_column(ui.chip, 8, 0);
    lv_obj_set_flex_flow(ui.chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui.chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    ui.chip_gps     = nav_make_label(ui.chip, NAV_FONT_CHIP, NAV_COL_TEXT_DIM, "");
    ui.chip_battery = nav_make_label(ui.chip, NAV_FONT_CHIP, NAV_COL_TEXT_DIM, "");
}

static void build_banner(lv_obj_t *parent)
{
    const int32_t w = fit_width(BANNER_Y, BANNER_W);

    ui.banner = lv_obj_create(parent);
    lv_obj_set_size(ui.banner, w, BANNER_H);
    lv_obj_align(ui.banner, LV_ALIGN_TOP_MID, 0, BANNER_Y);
    nav_style_panel(ui.banner);

    ui.banner_symbol = nav_make_label(ui.banner, NAV_FONT_TURN_SYMBOL, NAV_COL_ACCENT,
                                      LV_SYMBOL_GPS);
    lv_obj_align(ui.banner_symbol, LV_ALIGN_TOP_LEFT, 2, 0);

    ui.banner_distance = nav_make_label(ui.banner, NAV_FONT_TURN_DISTANCE, NAV_COL_TEXT, "");
    lv_obj_align(ui.banner_distance, LV_ALIGN_TOP_LEFT, 44, 0);

    ui.banner_street = nav_make_label(ui.banner, NAV_FONT_STREET, NAV_COL_TEXT_DIM, "");
    lv_obj_set_width(ui.banner_street, w - 28);
    lv_label_set_long_mode(ui.banner_street, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(ui.banner_street, LV_ALIGN_BOTTOM_LEFT, 2, 0);
}

/**
 * One "value over caption" column of the summary panel.
 *
 * The caption is handed back too: a drive and a workout want different figures
 * in the same three slots, so the headings are retitled at runtime. It also
 * carries the unit - at this size "45" over "km/h" reads instantly where
 * "45 km/h" on one line does not fit a third of the panel.
 */
static void build_summary_column(lv_obj_t *parent, lv_align_t align, int32_t x,
                                 lv_obj_t **out_value, lv_obj_t **out_caption)
{
    *out_value = nav_make_label(parent, NAV_FONT_VALUE, NAV_COL_TEXT, "--");
    lv_obj_align(*out_value, align, x, -9);

    *out_caption = nav_make_label(parent, NAV_FONT_CAPTION, NAV_COL_TEXT_DIM, "");
    lv_obj_align(*out_caption, align, x, 15);
}

static void build_summary(lv_obj_t *parent)
{
    ui.summary = lv_obj_create(parent);
    lv_obj_set_size(ui.summary, fit_width(NAV_SCREEN_SIZE + SUMMARY_Y, SUMMARY_W), SUMMARY_H);
    lv_obj_align(ui.summary, LV_ALIGN_BOTTOM_MID, 0, SUMMARY_Y);
    nav_style_panel(ui.summary);

    build_summary_column(ui.summary, LV_ALIGN_LEFT_MID,   2, &ui.val_left,  &ui.cap_left);
    build_summary_column(ui.summary, LV_ALIGN_CENTER,     0, &ui.val_mid,   &ui.cap_mid);
    build_summary_column(ui.summary, LV_ALIGN_RIGHT_MID, -2, &ui.val_right, &ui.cap_right);
}

/* ------------------------------------------------------------- demo bar */

#if NAV_DEMO_CONTROLS

static void demo_play_cb(lv_event_t *e)
{
    (void)e;
    bool now_paused = !gps_source_sim_is_paused();
    gps_source_sim_set_paused(now_paused);
    lv_label_set_text(ui.demo_play_label, now_paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    controls_show();
}

static void demo_speed_cb(lv_event_t *e)
{
    (void)e;
    ui.demo_speed_step = (ui.demo_speed_step + 1) % DEMO_SPEED_COUNT;
    float scale = DEMO_SPEEDS[ui.demo_speed_step];
    gps_source_sim_set_speed_scale(scale);

    char buf[8];
    snprintf(buf, sizeof(buf), "%.0fx", scale);
    lv_label_set_text(ui.demo_speed_label, buf);
    controls_show();
}

static void demo_skip_cb(lv_event_t *e)
{
    (void)e;
    /* No engine reset needed: the matcher falls back to a full scan when the
     * fix lands outside its search window, and the announcement stage re-arms
     * by itself as soon as the upcoming maneuver index changes. */
    gps_source_sim_skip_to_next_maneuver();
    map_view_set_follow(ui.map, true);
    controls_show();
}

static void demo_stray_cb(lv_event_t *e)
{
    (void)e;
    bool now = !gps_source_sim_is_straying();
    gps_source_sim_set_straying(now);
    /* Held down, in the off-route colour, so it is clear the demo is doing
     * this on purpose rather than the fix having gone wrong. */
    lv_obj_set_style_bg_color(ui.demo_stray_btn, now ? NAV_COL_BG_OFF : NAV_COL_BG, 0);
    controls_show();
}

static void demo_restart_cb(lv_event_t *e)
{
    (void)e;
    gps_source_sim_restart();
    map_view_set_follow(ui.map, true);
    controls_show();
}

static void build_demo_bar(lv_obj_t *parent)
{
    /* The transport drives the simulator along the route; with no route there
     * is nothing for it to drive. */
    if (!gps_source_is_simulated() || ui.map_only) return;

    ui.demo_bar = lv_obj_create(parent);
    lv_obj_set_size(ui.demo_bar, DEMO_BAR_W, DEMO_BAR_H);
    lv_obj_align(ui.demo_bar, LV_ALIGN_BOTTOM_MID, 0, DEMO_BAR_Y);
    nav_style_panel(ui.demo_bar);
    lv_obj_set_style_radius(ui.demo_bar, DEMO_BAR_H / 2, 0);
    lv_obj_set_style_pad_all(ui.demo_bar, 0, 0);

    const int32_t x0 = -(DEMO_BTN_SPACING * 4) / 2;

    make_round_button_sized(ui.demo_bar, LV_SYMBOL_PAUSE, demo_play_cb,
                            LV_ALIGN_CENTER, x0 + DEMO_BTN_SPACING * 0, 0,
                            DEMO_BTN_SIZE, NAV_FONT_CHIP, &ui.demo_play_label);

    make_round_button_sized(ui.demo_bar, "1x", demo_speed_cb,
                            LV_ALIGN_CENTER, x0 + DEMO_BTN_SPACING * 1, 0,
                            DEMO_BTN_SIZE, NAV_FONT_CHIP, &ui.demo_speed_label);

    make_round_button_sized(ui.demo_bar, LV_SYMBOL_NEXT, demo_skip_cb,
                            LV_ALIGN_CENTER, x0 + DEMO_BTN_SPACING * 2, 0,
                            DEMO_BTN_SIZE, NAV_FONT_CHIP, NULL);

    ui.demo_stray_btn =
        make_round_button_sized(ui.demo_bar, LV_SYMBOL_WARNING, demo_stray_cb,
                                LV_ALIGN_CENTER, x0 + DEMO_BTN_SPACING * 3, 0,
                                DEMO_BTN_SIZE, NAV_FONT_CHIP, NULL);

    make_round_button_sized(ui.demo_bar, LV_SYMBOL_REFRESH, demo_restart_cb,
                            LV_ALIGN_CENTER, x0 + DEMO_BTN_SPACING * 4, 0,
                            DEMO_BTN_SIZE, NAV_FONT_CHIP, NULL);
}

#else
static void build_demo_bar(lv_obj_t *parent) { (void)parent; }
#endif /* NAV_DEMO_CONTROLS */

/* ---------------------------------------------------------------- compass */

static const char *cardinal_name(float deg)
{
    static const char *const names[16] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW",
    };
    return names[(int)((deg + 11.25f) / 22.5f) % 16];
}

static bool compass_readout_shown(void)
{
    return !lv_obj_has_flag(ui.compass_value, LV_OBJ_FLAG_HIDDEN);
}

static void compass_fill_readout(void)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)lroundf(ui.compass_deg) % 360);
    lv_label_set_text(ui.compass_value, buf);

    /* While the gyroscope has gone too long without a GPS course, a direction
     * name would be a guess stated as fact, so say how far off it may be. */
    if (ui.compass_trusted) {
        lv_label_set_text(ui.compass_caption, cardinal_name(ui.compass_deg));
        lv_obj_set_style_text_color(ui.compass_caption, NAV_COL_TEXT_DIM, 0);
    } else {
        snprintf(buf, sizeof(buf), "\xC2\xB1%d\xC2\xB0", ui.compass_err_deg);
        lv_label_set_text(ui.compass_caption, buf);
        lv_obj_set_style_text_color(ui.compass_caption, NAV_COL_CAUTION, 0);
    }
}

static void compass_show_readout(bool show)
{
    set_hidden(ui.compass_value,   !show);
    set_hidden(ui.compass_caption, !show);
    if (show) compass_fill_readout();
    lv_obj_invalidate(ui.compass);
}

static void compass_readout_end_cb(lv_timer_t *t)
{
    compass_show_readout(false);
    lv_timer_delete(t);
    ui.compass_readout_timer = NULL;
}

/** A tap swaps the needle for the number, and a second tap swaps it back. */
static void compass_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (ui.compass_readout_timer) {
        lv_timer_delete(ui.compass_readout_timer);
        ui.compass_readout_timer = NULL;
    }

    if (compass_readout_shown()) {
        compass_show_readout(false);
        return;
    }

    compass_show_readout(true);
    ui.compass_readout_timer = lv_timer_create(compass_readout_end_cb, COMPASS_READOUT_MS, NULL);
    lv_timer_set_repeat_count(ui.compass_readout_timer, 1);
}

/** The needle: red toward north, grey away from it, and an N past the red tip. */
static void compass_draw_cb(lv_event_t *e)
{
    if (compass_readout_shown()) return;

    lv_layer_t *layer = lv_event_get_layer(e);
    if (!layer) return;

    lv_area_t a;
    lv_obj_get_coords(ui.compass, &a);
    double cx = (a.x1 + a.x2) / 2.0;
    double cy = (a.y1 + a.y2) / 2.0;

    /* The device points compass_deg clockwise of north, so north lies that far
     * anticlockwise of the top of the screen. */
    double th = -ui.compass_deg * M_PI / 180.0;
    double dx = sin(th), dy = -cos(th);     /* toward north */
    double px = cos(th), py = sin(th);      /* across the needle */

    /* Faded, not hidden, while the heading is uncertain: roughly right is
     * still useful, as long as it does not look as sure of itself as a
     * heading the GPS has just set. */
    lv_opa_t opa = ui.compass_trusted ? LV_OPA_COVER : LV_OPA_40;

    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.opa = opa;
    tri.p[1].x = (lv_value_precise_t)lround(cx + px * COMPASS_NEEDLE_W);
    tri.p[1].y = (lv_value_precise_t)lround(cy + py * COMPASS_NEEDLE_W);
    tri.p[2].x = (lv_value_precise_t)lround(cx - px * COMPASS_NEEDLE_W);
    tri.p[2].y = (lv_value_precise_t)lround(cy - py * COMPASS_NEEDLE_W);

    tri.color  = NAV_COL_TEXT_DIM;
    tri.p[0].x = (lv_value_precise_t)lround(cx - dx * COMPASS_NEEDLE_LEN);
    tri.p[0].y = (lv_value_precise_t)lround(cy - dy * COMPASS_NEEDLE_LEN);
    lv_draw_triangle(layer, &tri);

    tri.color  = NAV_COL_NORTH;
    tri.p[0].x = (lv_value_precise_t)lround(cx + dx * COMPASS_NEEDLE_LEN);
    tri.p[0].y = (lv_value_precise_t)lround(cy + dy * COMPASS_NEEDLE_LEN);
    lv_draw_triangle(layer, &tri);

    lv_draw_rect_dsc_t pivot;
    lv_draw_rect_dsc_init(&pivot);
    pivot.radius   = LV_RADIUS_CIRCLE;
    pivot.bg_color = NAV_COL_TEXT;
    pivot.bg_opa   = opa;
    int32_t pcx = (int32_t)lround(cx), pcy = (int32_t)lround(cy);
    lv_area_t pa = { .x1 = pcx - 2, .y1 = pcy - 2, .x2 = pcx + 2, .y2 = pcy + 2 };
    lv_draw_rect(layer, &pivot, &pa);

    lv_draw_label_dsc_t n;
    lv_draw_label_dsc_init(&n);
    n.text  = "N";
    n.font  = NAV_FONT_CAPTION;
    n.color = NAV_COL_NORTH;
    n.opa   = opa;
    n.align = LV_TEXT_ALIGN_CENTER;

    int32_t lh = lv_font_get_line_height(n.font);
    int32_t nx = (int32_t)lround(cx + dx * COMPASS_N_RADIUS);
    int32_t ny = (int32_t)lround(cy + dy * COMPASS_N_RADIUS);
    lv_area_t na = { .x1 = nx - 8, .y1 = ny - lh / 2, .x2 = nx + 8, .y2 = ny - lh / 2 + lh - 1 };
    lv_draw_label(layer, &n, &na);
}

static void build_compass(lv_obj_t *parent)
{
    ui.compass = lv_obj_create(parent);
    lv_obj_set_size(ui.compass, COMPASS_SIZE, COMPASS_SIZE);
    lv_obj_align(ui.compass, LV_ALIGN_CENTER, COMPASS_DX, COMPASS_DY);
    nav_style_panel(ui.compass);
    lv_obj_set_style_radius(ui.compass, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(ui.compass, 0, 0);

    /* nav_style_panel() clears CLICKABLE; the dial is a button for its readout,
     * and being on top of the map it keeps its taps from toggling the controls. */
    lv_obj_add_flag(ui.compass, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(ui.compass, NAV_COL_ACCENT, LV_STATE_PRESSED);
    lv_obj_add_event_cb(ui.compass, compass_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui.compass, compass_draw_cb, LV_EVENT_DRAW_MAIN_END, NULL);

    ui.compass_value = nav_make_label(ui.compass, NAV_FONT_STREET, NAV_COL_TEXT, "");
    lv_obj_align(ui.compass_value, LV_ALIGN_CENTER, 0, -7);
    lv_obj_add_flag(ui.compass_value, LV_OBJ_FLAG_HIDDEN);

    ui.compass_caption = nav_make_label(ui.compass, NAV_FONT_CAPTION, NAV_COL_TEXT_DIM, "");
    lv_obj_align(ui.compass_caption, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_flag(ui.compass_caption, LV_OBJ_FLAG_HIDDEN);

    /* Hidden until the GPS has set north: before that there is nothing true
     * for the needle to say. */
    lv_obj_add_flag(ui.compass, LV_OBJ_FLAG_HIDDEN);
}

void nav_ui_update_compass(const nav_compass_t *c)
{
    if (!ui.compass) return;

    if (!c || !c->valid) {
        if (!lv_obj_has_flag(ui.compass, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(ui.compass, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    bool  trusted = c->error_deg <= NAV_COMPASS_TRUST_DEG;
    int   err_deg = (int)lroundf(c->error_deg);
    bool  hidden  = lv_obj_has_flag(ui.compass, LV_OBJ_FLAG_HIDDEN);
    float turned  = (float)fabs(map_geo_angle_diff_deg(ui.compass_deg, c->heading_deg));

    if (!hidden && trusted == ui.compass_trusted && err_deg == ui.compass_err_deg &&
        turned < COMPASS_REDRAW_DEG) {
        return;
    }

    ui.compass_deg     = c->heading_deg;
    ui.compass_err_deg = err_deg;
    ui.compass_trusted = trusted;

    lv_obj_set_style_border_color(ui.compass, trusted ? NAV_COL_BORDER : NAV_COL_CAUTION, 0);
    if (hidden) lv_obj_remove_flag(ui.compass, LV_OBJ_FLAG_HIDDEN);
    if (compass_readout_shown()) compass_fill_readout();
    lv_obj_invalidate(ui.compass);
}

/**
 * What both screens share: the map on @p folder, clamped to the zoom levels
 * the card holds, and every panel and button over it. A route, when there is
 * one, is drawn and the map opens at its start.
 */
static bool build_screen(lv_obj_t *parent, map_route_handle_t route,
                         const char *folder, bool map_only)
{
    memset(&ui, 0, sizeof(ui));
    ui.map_only = map_only;

    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);

    ui.map = map_view_create(parent);
    if (!ui.map) {
        ESP_LOGE(TAG, "Cannot create the map view");
        return false;
    }

    lv_obj_set_size(ui.map, NAV_SCREEN_SIZE, NAV_SCREEN_SIZE);
    lv_obj_align(ui.map, LV_ALIGN_CENTER, 0, 0);

    /* map_view measures its content area, which is still empty until LVGL runs
     * the layout. Force it now rather than at the next frame. */
    lv_obj_update_layout(ui.map);

    /* The route and marker are drawn in pixels, and this panel has few of
     * them. Left at map_view's defaults the road becomes a stripe wide enough
     * to hide the junction it is going through. */
    map_view_style_t style;
    map_view_get_style(ui.map, &style);
    style.route_width   = 7;
    style.casing_width  = 11;
    style.marker_radius = 11;
    map_view_set_style(ui.map, &style);

    if (!map_view_set_tile_source(ui.map, NAV_SD_MOUNT_POINT, folder,
                                  NAV_TILE_CACHE_SLOTS, true)) {
        ESP_LOGE(TAG, "Cannot open the tile cache at %s/%s",
                 NAV_SD_MOUNT_POINT, folder);
        return false;
    }
    ESP_LOGI(TAG, "Tiles from %s/%s", NAV_SD_MOUNT_POINT, folder);

    /* Clamp the view to the zoom levels the card actually holds. A corridor
     * download covers a few levels and nothing else; asking for one outside
     * that range is not an error anywhere in the stack, it just draws
     * placeholders, which reads as a broken map rather than a missing
     * download. */
    int card_min = 0, card_max = 0;
    int levels = map_tile_cache_probe_zooms(NAV_SD_MOUNT_POINT, folder,
                                            &card_min, &card_max);
    int zmin = NAV_ZOOM_MIN;
    int zmax = NAV_ZOOM_MAX;
    if (levels > 0) {
        zmin = LV_MAX(NAV_ZOOM_MIN, card_min);
        zmax = LV_MIN(NAV_ZOOM_MAX, card_max);
        if (zmin > zmax) {
            /* The card and nav_config.h do not overlap at all. Trust the card. */
            ESP_LOGW(TAG, "nav_config.h asks for z%d-%d but the card has z%d-%d; "
                          "using the card's range",
                     NAV_ZOOM_MIN, NAV_ZOOM_MAX, card_min, card_max);
            zmin = card_min;
            zmax = card_max;
        }
    } else {
        ESP_LOGE(TAG, "No tiles under %s/%s - the map will be blank",
                 NAV_SD_MOUNT_POINT, folder);
    }

    int zstart = NAV_ZOOM_DEFAULT;
    if (zstart < zmin) zstart = zmin;
    if (zstart > zmax) zstart = zmax;
    ESP_LOGI(TAG, "Map zoom range z%d-%d, opening at z%d", zmin, zmax, zstart);

    map_view_set_zoom_range(ui.map, zmin, zmax);
    map_view_set_zoom(ui.map, zstart);
    map_view_set_follow_offset(ui.map, NAV_FOLLOW_OFFSET_PX);
    map_view_set_debug_overlay(ui.map, NAV_SHOW_MAP_DEBUG_OVERLAY);
    lv_obj_add_event_cb(ui.map, map_follow_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ui.map, map_input_cb, LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(ui.map, map_input_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(ui.map, map_input_cb, LV_EVENT_RELEASED, NULL);

    if (route) {
        map_view_set_route(ui.map, route);

        /* Open where the drive begins. Framing the whole route instead would
         * be a nicer first impression, but map_view_fit_bounds() would pick a
         * zoom several levels below anything a corridor download contains -
         * a 12 km route lands on z12 - and the driver would be looking at an
         * empty grid. */
        double start_lat, start_lon;
        if (map_route_get_start(route, &start_lat, &start_lon)) {
            map_view_set_center(ui.map, start_lat, start_lon);
            map_view_set_position(ui.map, start_lat, start_lon, 0, false);
        }
    }

    build_chip(parent);
    build_banner(parent);
    build_summary(parent);

    ui.btn_zoom_in  = make_round_button(parent, LV_SYMBOL_PLUS,  zoom_in_cb,
                                        LV_ALIGN_RIGHT_MID, -BTN_EDGE, -BTN_STACK);
    ui.btn_zoom_out = make_round_button(parent, LV_SYMBOL_MINUS, zoom_out_cb,
                                        LV_ALIGN_RIGHT_MID, -BTN_EDGE,  BTN_STACK);
    ui.btn_routes   = make_round_button(parent, LV_SYMBOL_LIST,  routes_cb,
                                        LV_ALIGN_LEFT_MID,   BTN_EDGE, -BTN_STACK);
    ui.btn_recenter = make_round_button(parent, LV_SYMBOL_GPS,   recenter_cb,
                                        LV_ALIGN_LEFT_MID,   BTN_EDGE,  BTN_STACK);

    build_compass(parent);
    build_demo_bar(parent);

    /* Follow from the first frame. Everything below the map is now built, so
     * the VALUE_CHANGED this raises reaches a button that exists. */
    map_view_set_follow(ui.map, true);

    /* Up at start-up, so the controls are discoverable; they drop away on
     * their own a few seconds later. */
    controls_show();
    return true;
}

bool nav_ui_create(lv_obj_t *parent, map_route_handle_t route)
{
    /* A route can name the folder its tiles are in, so one card can carry
     * routes in different regions. Empty means "use the configured default". */
    const char *folder = route ? map_route_get_tile_folder(route) : "";
    if (!folder || folder[0] == '\0') folder = NAV_TILE_FOLDER;

    if (!build_screen(parent, route, folder, false)) return false;

    ESP_LOGI(TAG, "Navigation UI ready (%dx%d round), following at offset %d px",
             NAV_SCREEN_SIZE, NAV_SCREEN_SIZE, NAV_FOLLOW_OFFSET_PX);
    return true;
}

bool nav_ui_create_map_only(lv_obj_t *parent, const char *folder,
                            const nav_map_extent_t *extent)
{
    if (!folder || folder[0] == '\0') folder = NAV_TILE_FOLDER;

    if (!build_screen(parent, NULL, folder, true)) return false;

    /* Nothing to follow, so the banner has nothing to say, and the summary
     * waits for a fix to have something to count. */
    set_hidden(ui.banner, true);
    set_hidden(ui.summary, true);

    if (extent) {
        ui.extent       = *extent;
        ui.have_extent  = true;
        ui.follow_armed = true;
        center_on_extent();
        map_view_set_follow(ui.map, false);
    }

    ESP_LOGI(TAG, "Map-only UI ready on %s/%s%s", NAV_SD_MOUNT_POINT, folder,
             extent ? ", opening on its tiles" : ", following the first fix");
    return true;
}

lv_obj_t *nav_ui_get_map(void)
{
    return ui.map;
}

void nav_ui_set_change_route_cb(nav_ui_change_route_cb_t cb, void *ctx)
{
    ui.change_route_cb  = cb;
    ui.change_route_ctx = ctx;
}

void nav_ui_destroy(void)
{
    /* These timers are not children of the screen, so deleting the screen would
     * leave them running and they would fire on freed widgets. */
    if (ui.flash_timer)     { lv_timer_delete(ui.flash_timer);     ui.flash_timer = NULL; }
    if (ui.controls_timer)  { lv_timer_delete(ui.controls_timer);  ui.controls_timer = NULL; }
    if (ui.compass_readout_timer) {
        lv_timer_delete(ui.compass_readout_timer);
        ui.compass_readout_timer = NULL;
    }
    memset(&ui, 0, sizeof(ui));
}

/* ------------------------------------------------------------------ update */

static void flash_end_cb(lv_timer_t *t)
{
    lv_obj_set_style_border_color(ui.banner, NAV_COL_BORDER, 0);
    lv_obj_set_style_border_width(ui.banner, 2, 0);
    lv_timer_delete(t);
    ui.flash_timer = NULL;
}

/** Briefly outline the banner so a new instruction catches the eye. */
static void flash_banner(lv_color_t color)
{
    lv_obj_set_style_border_color(ui.banner, color, 0);
    lv_obj_set_style_border_width(ui.banner, 4, 0);

    if (ui.flash_timer) lv_timer_delete(ui.flash_timer);
    ui.flash_timer = lv_timer_create(flash_end_cb, CUE_FLASH_MS, NULL);
    lv_timer_set_repeat_count(ui.flash_timer, 1);
}

/** The banner arrow points where the road is, relative to where you face. */
static const char *return_arrow_symbol(double rel_deg)
{
    double a = rel_deg < 0 ? -rel_deg : rel_deg;
    if (a <= 25.0)  return LV_SYMBOL_UP;
    if (a >= 155.0) return LV_SYMBOL_DOWN;
    return rel_deg > 0 ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT;
}

static void set_banner(lv_color_t bg, lv_color_t symbol_color,
                       const char *symbol, const char *big, const char *sub)
{
    lv_obj_set_style_bg_color(ui.banner, bg, 0);
    lv_obj_set_style_text_color(ui.banner_symbol, symbol_color, 0);
    lv_label_set_text(ui.banner_symbol, symbol);
    lv_label_set_text(ui.banner_distance, big);
    lv_label_set_text(ui.banner_street, sub);
}

static const char *battery_symbol(int percent)
{
    if (percent >= 90) return LV_SYMBOL_BATTERY_FULL;
    if (percent >= 65) return LV_SYMBOL_BATTERY_3;
    if (percent >= 40) return LV_SYMBOL_BATTERY_2;
    if (percent >= 15) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void update_chip(const nav_state_t *st)
{
    char buf[48];

    if (gps_source_is_simulated() && ui.map_only) {
        /* The simulator drives along a route. With none it reports nothing,
         * and the chip should not look as if it were playing. */
        snprintf(buf, sizeof(buf), "DEMO no route");
    } else if (gps_source_is_simulated()) {
        /* Never let a demo be mistaken for a real fix. */
        snprintf(buf, sizeof(buf), "%s DEMO %.0fx",
                 gps_source_sim_is_paused() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY,
                 gps_source_sim_get_speed_scale());
    } else if (st->phase == NAV_PHASE_ACQUIRING) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " searching");
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_GPS " %u", (unsigned)st->satellites);
    }
    lv_label_set_text(ui.chip_gps, buf);

    nav_power_state_t pw;
    nav_power_get(&pw);

    if (!pw.present || pw.percent < 0) {
        /* No cell, or no gauge reading yet: show the charger state if there is
         * one and otherwise say nothing, rather than a placeholder percentage. */
        if (pw.vbus) {
            lv_label_set_text(ui.chip_battery, LV_SYMBOL_USB);
            lv_obj_set_style_text_color(ui.chip_battery, NAV_COL_TEXT_DIM, 0);
        } else {
            lv_label_set_text(ui.chip_battery, "");
        }
        return;
    }

    snprintf(buf, sizeof(buf), "%s %d%%",
             pw.charging ? LV_SYMBOL_CHARGE : battery_symbol(pw.percent), pw.percent);
    lv_label_set_text(ui.chip_battery, buf);
    lv_obj_set_style_text_color(ui.chip_battery,
                                (!pw.charging && pw.percent <= NAV_BATTERY_LOW_PCT)
                                    ? NAV_COL_WARN : NAV_COL_TEXT_DIM, 0);
}

static void set_column(lv_obj_t *value, lv_obj_t *caption,
                       const char *text, const char *heading)
{
    lv_label_set_text(value, text);
    lv_label_set_text(caption, heading);
}

/**
 * Speed, split into a number and its unit.
 *
 * A third of the summary panel is 82 px, which "45 km/h" at 20 px does not
 * fit. The unit goes in the caption slot, where the column headings live
 * anyway, and the number gets the space.
 */
static void format_speed(double mps, char *buf, size_t n, const char **out_unit)
{
#if NAV_USE_IMPERIAL
    snprintf(buf, n, "%.0f", mps * 2.2369363);
    *out_unit = "mph";
#else
    snprintf(buf, n, "%.0f", mps * 3.6);
    *out_unit = "km/h";
#endif
}

static void update_summary(const nav_state_t *st, bool show)
{
    if (!show) {
        lv_obj_add_flag(ui.summary, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(ui.summary, LV_OBJ_FLAG_HIDDEN);

    char a[32], b[32], c[32];
    const char *speed_unit = "";
    format_speed(st->speed_mps, c, sizeof(c), &speed_unit);

    if (!st->on_route && st->phase == NAV_PHASE_OFF_ROUTE) {
        /* Distance remaining is frozen while off route, so showing it would be
         * a number that does not move. The gap to the road does move, and is
         * the one the rider is watching. */
        map_geo_format_distance(st->cross_track_m, NAV_USE_IMPERIAL, a, sizeof(a));
        map_geo_format_duration(st->off_route_s, b, sizeof(b));
        set_column(ui.val_left, ui.cap_left, a, "off route");
        set_column(ui.val_mid,  ui.cap_mid,  b, "for");
    } else if (st->kind == MAP_ROUTE_KIND_EXERCISE || st->phase == NAV_PHASE_FREE) {
        /* A workout counts up. What matters is how far you have gone and how
         * long you have been going, not when you will be finished - and with
         * no route there is no finish to count down to anyway. */
        map_geo_format_distance(st->session_m, NAV_USE_IMPERIAL, a, sizeof(a));
        map_geo_format_duration(st->elapsed_s, b, sizeof(b));
        set_column(ui.val_left,  ui.cap_left,  a, "distance");
        set_column(ui.val_mid,   ui.cap_mid,   b, "elapsed");
    } else {
        map_geo_format_distance(st->remaining_m, NAV_USE_IMPERIAL, a, sizeof(a));
        map_geo_format_duration(st->eta_s, b, sizeof(b));
        set_column(ui.val_left,  ui.cap_left,  a, "remaining");
        set_column(ui.val_mid,   ui.cap_mid,   b, "arrive in");
    }
    set_column(ui.val_right, ui.cap_right, c, speed_unit);
}

/**
 * A map on its own: where you are and the outing so far, and nothing about a
 * route. The banner stays hidden throughout.
 */
static void update_map_only(const nav_state_t *st)
{
    if (st->phase != NAV_PHASE_FREE) {          /* no fix yet */
        update_summary(st, false);
        return;
    }

    ui.have_fix = true;
    update_summary(st, true);
    map_view_set_position(ui.map, st->lat, st->lon, st->heading_deg, true);

    if (ui.follow_armed &&
        (!ui.have_extent || nav_map_extent_contains(&ui.extent, st->lat, st->lon))) {
        ui.follow_armed = false;
        map_view_set_follow(ui.map, true);
    }
}

void nav_ui_update(const nav_state_t *st)
{
    if (!st || !ui.map) return;

    update_chip(st);

    if (ui.map_only) {
        update_map_only(st);
        return;
    }

    switch (st->phase) {
        case NAV_PHASE_NO_ROUTE:
            set_banner(NAV_COL_BG, NAV_COL_TEXT_DIM, LV_SYMBOL_WARNING, "No route",
                       "Copy a route to the SD card");
            update_summary(st, false);
            return;

        case NAV_PHASE_ACQUIRING:
            set_banner(NAV_COL_BG, NAV_COL_ACCENT, LV_SYMBOL_GPS, "Acquiring",
                       "Waiting for a position fix");
            update_summary(st, false);
            return;

        case NAV_PHASE_ARRIVED:
            set_banner(NAV_COL_BG_DONE, NAV_COL_TEXT, LV_SYMBOL_OK, "Arrived",
                       "You have reached your destination");
            break;

        case NAV_PHASE_OFF_ROUTE: {
            /* "Off route" on its own leaves the rider to guess. Give the two
             * facts they can act on: how far the road is, and which way. */
            char dist[32], sub[96];
            map_geo_format_distance(st->cross_track_m, NAV_USE_IMPERIAL, dist, sizeof(dist));

            const char *trend = (st->off_route_trend < 0) ? "  " LV_SYMBOL_OK " closing"
                              : (st->off_route_trend > 0) ? "  " LV_SYMBOL_WARNING " further"
                              : "";
            snprintf(sub, sizeof(sub), "Route is %s%s", st->return_hint, trend);

            set_banner(NAV_COL_BG_OFF, NAV_COL_TEXT,
                       return_arrow_symbol(st->return_rel_deg), dist, sub);
            break;
        }

        case NAV_PHASE_NAVIGATING:
        default: {
            /* On a loop the lap number is the thing you keep glancing at, so it
             * leads the sub-line ahead of the street name. */
            char sub[80];
            const char *street = (st->has_maneuver && st->maneuver_street[0])
                                 ? st->maneuver_street
                                 : (st->has_maneuver
                                    ? map_maneuver_type_name(st->maneuver_type)
                                    : "Follow the highlighted road");
            if (st->is_loop) {
                snprintf(sub, sizeof(sub), "Lap %u  " LV_SYMBOL_BULLET "  %s",
                         (unsigned)(st->lap + 1), street);
            } else {
                snprintf(sub, sizeof(sub), "%s", street);
            }

            if (st->has_maneuver) {
                char dist[32];
                map_geo_format_distance(st->dist_to_maneuver_m, NAV_USE_IMPERIAL,
                                        dist, sizeof(dist));
                set_banner(NAV_COL_BG, NAV_COL_ACCENT,
                           map_maneuver_type_symbol(st->maneuver_type), dist, sub);
            } else {
                set_banner(NAV_COL_BG, NAV_COL_ACCENT, LV_SYMBOL_UP, "Continue", sub);
            }
            break;
        }
    }

    update_summary(st, true);

    map_view_set_position(ui.map, st->lat, st->lon, st->heading_deg, true);
    map_view_set_progress(ui.map, st->traveled_m);
    map_view_set_off_route(ui.map, !st->on_route);
    map_view_set_return_target(ui.map, st->return_lat, st->return_lon, !st->on_route);

    switch (st->cue) {
        case NAV_CUE_MANEUVER_NOW:
        case NAV_CUE_MANEUVER_NEAR:
        case NAV_CUE_MANEUVER_FAR:
            flash_banner(NAV_COL_ACCENT);
            break;
        case NAV_CUE_OFF_ROUTE:
        case NAV_CUE_OFF_ROUTE_FAR:
        case NAV_CUE_OFF_ROUTE_AGAIN:
            flash_banner(NAV_COL_WARN);
            break;
        case NAV_CUE_ARRIVED:
        case NAV_CUE_LAP:
            flash_banner(NAV_COL_GOOD);
            break;
        default:
            break;
    }
}
