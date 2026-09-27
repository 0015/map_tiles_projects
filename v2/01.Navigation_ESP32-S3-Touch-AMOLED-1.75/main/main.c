/**
 * Offline turn-by-turn navigation on an ESP32-S3 with a 466x466 round AMOLED.
 *
 * Board : Waveshare ESP32-S3-Touch-AMOLED-1.75 (1.75 inch round panel)
 * Needs : an SD card holding
 *           /sdcard/tiles1/<z>/<x>/<y>.bin   256x256 RGB565 tiles
 *           /sdcard/routes/<name>.bin        produced by OfflineMapDownloader v2
 *         Routes are optional: a card with only tiles on it opens as a map,
 *         with your position and nothing to follow.
 *
 * The guidance engine, the position source and the route loader are plain C
 * that knows nothing about this board. What is board-specific lives here and
 * in nav_ui.c.
 *
 * Threading: the GPS source runs in its own task and only drops a fix into a
 * mutex-protected slot. An LVGL timer picks it up, so everything that touches a
 * widget runs on the LVGL thread. The compass works the same way, with a timer
 * of its own, because it keeps reporting whether or not there is a fix.
 */

#include <string.h>

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "bsp/esp-bsp.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "lvgl.h"

#include "map_route.h"
#include "nav_config.h"
#include "nav_engine.h"
#include "nav_ui.h"
#include "nav_routes.h"
#include "nav_maps.h"
#include "nav_power.h"
#include "nav_theme.h"
#include "gps_source.h"
#include "compass_source.h"

static const char *TAG = "nav";

#define FIX_POLL_PERIOD_MS      100
#define COMPASS_POLL_PERIOD_MS  50

static map_route_handle_t s_route;
static lv_obj_t          *s_nav_screen;
static lv_timer_t        *s_fix_timer;
static lv_timer_t        *s_compass_timer;

/* Everything on the card that can be opened: the routes, then the tile
 * folders as maps on their own. */
static nav_route_entry_t  s_entries[NAV_MAX_ROUTES];
static int                s_entry_count;

static TaskHandle_t s_touch_task;

static SemaphoreHandle_t s_fix_lock;
static nav_fix_t         s_latest_fix;
static volatile uint32_t s_fix_seq;
static uint32_t          s_seen_seq;

/* ------------------------------------------------------------- heartbeat
 *
 * A screen that has stopped and a log that says nothing leaves no way to tell
 * which half of the firmware is at fault. This costs one line every
 * NAV_HEARTBEAT_S and answers that: the counter is raised by a timer on the
 * LVGL thread and read by a task that is not, so if the line keeps coming and
 * reports STUCK, the UI thread is blocked - and if the line stops too, it is
 * everything else.
 */
#if NAV_HEARTBEAT_S > 0

static volatile uint32_t s_lvgl_beats;

static void lvgl_beat_cb(lv_timer_t *t)
{
    (void)t;
    s_lvgl_beats++;
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    uint32_t last = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(NAV_HEARTBEAT_S * 1000));

        uint32_t beats = s_lvgl_beats;
        ESP_LOGI(TAG, "alive: LVGL %s, %u fixes, heap %u KB internal / %u KB PSRAM",
                 (beats != last) ? "running" : "STUCK",
                 (unsigned)s_fix_seq,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        last = beats;
    }
}

static void heartbeat_start(void)
{
    bsp_display_lock(0);
    lv_timer_create(lvgl_beat_cb, 500, NULL);
    bsp_display_unlock();
    xTaskCreate(heartbeat_task, "nav_beat", 3072, NULL, 1, NULL);
}

#else
static void heartbeat_start(void) { }
#endif

/** Called from the GPS task. Keep it short and free of LVGL. */
static void on_fix(const nav_fix_t *fix, void *ctx)
{
    (void)ctx;

    /* North comes from the course, so the compass hears every fix, including
     * any the UI timer would skip. It only copies it. */
    compass_source_feed_course(fix->valid, fix->speed_mps, fix->heading_deg);

    if (xSemaphoreTake(s_fix_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    s_latest_fix = *fix;
    s_fix_seq++;
    xSemaphoreGive(s_fix_lock);
}

static void fix_timer_cb(lv_timer_t *t)
{
    (void)t;

    if (s_fix_seq == s_seen_seq) return;

    nav_fix_t fix;
    if (xSemaphoreTake(s_fix_lock, 0) != pdTRUE) return;
    fix = s_latest_fix;
    s_seen_seq = s_fix_seq;
    xSemaphoreGive(s_fix_lock);

    /* Stopped, the GPS course means nothing, but the compass still knows which
     * way the device faces. Only while it is sure of it: the same bar the dial
     * sets for drawing its needle at full strength. */
    nav_compass_t compass;
    bool facing = compass_source_get(&compass) &&
                  compass.error_deg <= NAV_COMPASS_TRUST_DEG;
    nav_engine_set_facing(compass.heading_deg, facing);

    nav_state_t state;
    nav_engine_update(&fix, &state);
    nav_ui_update(&state);

    if (state.cue != NAV_CUE_NONE) {
        ESP_LOGI(TAG, "%s | cue %d | %u m to turn | %u m left",
                 nav_phase_name(state.phase), (int)state.cue,
                 (unsigned)state.dist_to_maneuver_m, (unsigned)state.remaining_m);
    }
}

static void compass_timer_cb(lv_timer_t *t)
{
    (void)t;
    nav_compass_t compass;
    compass_source_get(&compass);
    nav_ui_update_compass(&compass);
}

/** Full-screen message for the failures that make navigation impossible. */
static void show_fatal(const char *title, const char *detail)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, NAV_COL_BG, 0);

    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_size(box, 300, 200);
    lv_obj_center(box);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = nav_make_label(box, NAV_FONT_VALUE, NAV_COL_WARN, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *d = nav_make_label(box, NAV_FONT_CHIP, NAV_COL_TEXT_DIM, "");
    lv_obj_set_width(d, 280);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(d, detail);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 52);
}


/* ------------------------------------------------------- switching routes */

static void start_navigation(const nav_route_entry_t *entry);
static void on_route_picked(const nav_route_entry_t *entry, void *ctx);

/** Tear the navigation screen down and go back to the list. */
static void on_change_route(void *ctx)
{
    (void)ctx;
    if (s_entry_count <= 0) return;

    ESP_LOGI(TAG, "Returning to the route list");

    /* Order matters: stop the fixes first so nothing is mid-update, then the
     * timer that consumes them, then the screen, and only then the route the
     * map was still pointing at. */
    gps_source_stop();

    if (s_fix_timer) {
        lv_timer_delete(s_fix_timer);
        s_fix_timer = NULL;
    }
    if (s_compass_timer) {
        lv_timer_delete(s_compass_timer);
        s_compass_timer = NULL;
    }
    nav_ui_destroy();

    lv_obj_t *old_screen = s_nav_screen;
    s_nav_screen = NULL;

    nav_routes_show_picker(s_entries, s_entry_count, on_route_picked, NULL);

    if (old_screen) lv_obj_delete(old_screen);

    if (s_route) {
        map_route_free(s_route);
        s_route = NULL;
    }
    s_seen_seq = s_fix_seq;
}

static void on_route_picked(const nav_route_entry_t *entry, void *ctx)
{
    (void)ctx;
    lv_obj_t *picker = nav_routes_get_screen();
    start_navigation(entry);

    /* Only once the new screen is up. If it could not be built, the failure
     * was drawn onto the picker - still the screen showing - and deleting it
     * would leave the display with nothing on it at all. */
    if (picker && s_nav_screen && picker != s_nav_screen) lv_obj_delete(picker);
}

/**
 * Open what was picked - a route to follow, or a tile folder as a map on its
 * own - build its screen, and start the position source.
 */
static void start_navigation(const nav_route_entry_t *entry)
{
    nav_map_extent_t extent;
    bool have_extent = false;

    if (entry->map_only) {
        s_route = NULL;
        nav_engine_init_free();
        /* Where the tiles are, from the folder itself, so the map opens on
         * them rather than at 0,0 while there is no fix. */
        have_extent = nav_maps_extent(entry->info.tile_folder, entry->info.min_zoom, &extent);
    } else {
        s_route = map_route_load(entry->path, true);
        if (!s_route) {
            show_fatal("Route unreadable", entry->path);
            return;
        }
        nav_engine_init(s_route);
    }

    s_nav_screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_nav_screen, LV_OBJ_FLAG_SCROLLABLE);

    bool ui_ok = entry->map_only
        ? nav_ui_create_map_only(s_nav_screen, entry->info.tile_folder,
                                 have_extent ? &extent : NULL)
        : nav_ui_create(s_nav_screen, s_route);
    if (!ui_ok) {
        lv_obj_delete(s_nav_screen);
        s_nav_screen = NULL;
        if (s_route) {
            nav_engine_init(NULL);      /* it must not keep the route it had */
            map_route_free(s_route);
            s_route = NULL;
        }
        show_fatal("Map unavailable",
                   entry->map_only ? "The tile folder could not be opened."
                                   : "The tile folder this route asks for could not be opened.");
        return;
    }
    nav_ui_set_change_route_cb(on_change_route, NULL);
    lv_screen_load(s_nav_screen);

    /* Draw the initial state before any fix arrives. */
    nav_state_t initial;
    nav_engine_update(NULL, &initial);
    nav_ui_update(&initial);

    s_fix_timer     = lv_timer_create(fix_timer_cb, FIX_POLL_PERIOD_MS, NULL);
    s_compass_timer = lv_timer_create(compass_timer_cb, COMPASS_POLL_PERIOD_MS, NULL);

    /* With no route the simulator has nothing to drive along and says so; the
     * map is still there to look at. */
    if (!gps_source_start(s_route, on_fix, NULL)) {
        ESP_LOGE(TAG, "Position source (%s) failed to start", gps_source_name());
    }

    ESP_LOGI(TAG, "%s %s with %s. PSRAM free %u KB, largest block %u KB, "
                  "internal free %u KB",
             entry->map_only ? "Showing the map" : "Navigating",
             entry->info.name, gps_source_name(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

/* ---------------------------------------------------------------- touch
 *
 * The CST9217 is driven here rather than through esp_lcd_touch and the LVGL
 * port. Three things in that stack made it unusable on this board, and the
 * last one is why none of it is left.
 *
 * Its pointer device is ESP_ERROR_CHECK(esp_lcd_touch_read_data(...)), so one
 * malformed frame aborts the firmware. The CST9217 driver reads the chip id at
 * start-up by entering the controller's command mode and never leaves it, and
 * frames read in that mode carry no marker - so the first read after the
 * screen goes live is exactly such a frame.
 *
 * And esp_lcd's I2C transport passes -1 as its transaction timeout: wait
 * forever. The vendor stack makes that call from the LVGL thread, so a wedged
 * bus takes the user interface with it, silently. Moving the read to a task of
 * its own kept the map alive and let the UI report what had happened:
 *
 *     W nav: Touch poller has not reported for 3000 ms - the bus is stuck.
 *
 * That is the measurement this driver exists for. The register and the frame
 * layout are the vendor's; the transport is the ordinary I2C master API with a
 * deadline on every transfer, so a stuck bus is an error that the poller can
 * recover from rather than a task that never returns.
 */

#define TOUCH_I2C_ADDR      0x5A
#define TOUCH_DATA_REG      { 0xD0, 0x00 }   /* 16-bit register address, big endian */
#define TOUCH_FRAME_BYTES   10               /* one point: 1 * 5 + 5 */
#define TOUCH_ACK_BYTE      0xAB             /* frame[6], or the frame is not one */
#define TOUCH_STATUS_DOWN   0x06             /* low nibble of frame[0] */
#define TOUCH_ACK_INDEX     6

static i2c_master_dev_handle_t s_touch_dev;

static portMUX_TYPE s_touch_mux = portMUX_INITIALIZER_UNLOCKED;

static struct {
    int32_t  x;
    int32_t  y;
    bool     pressed;
    uint32_t seq;       /**< Bumped after every poll, so a stall is visible */
} s_touch_state;

static void touch_reset_pulse(void)
{
    /* Timing from the vendor driver. Only ever called from the poller, so it
     * cannot land in the middle of somebody else's transfer. */
    gpio_set_level(BSP_LCD_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BSP_LCD_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/**
 * Read one touch frame.
 *
 * Two transfers, as the vendor driver does it: the register address, a short
 * pause the controller wants, then the frame. Both have a deadline.
 */
static esp_err_t touch_read_frame(uint8_t *frame)
{
    static const uint8_t reg[2] = TOUCH_DATA_REG;

    esp_err_t err = i2c_master_transmit(s_touch_dev, reg, sizeof(reg),
                                        NAV_TOUCH_I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;

    vTaskDelay(pdMS_TO_TICKS(2));

    err = i2c_master_receive(s_touch_dev, frame, TOUCH_FRAME_BYTES,
                             NAV_TOUCH_I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;

    /* An idle or confused controller answers with something that is not a
     * frame. Saying so is the caller's business, not a reason to shout. */
    if (frame[TOUCH_ACK_INDEX] != TOUCH_ACK_BYTE) return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

/** Pull the one point out of a frame, in screen coordinates. */
static bool touch_decode(const uint8_t *frame, int32_t *out_x, int32_t *out_y)
{
    if ((frame[5] & 0x7F) == 0) return false;
    if ((frame[0] & 0x0F) != TOUCH_STATUS_DOWN) return false;

    int32_t x = ((int32_t)frame[1] << 4) | (frame[3] >> 4);
    int32_t y = ((int32_t)frame[2] << 4) | (frame[3] & 0x0F);

    /* The digitiser is mounted turned about relative to the panel. These are
     * the mirror flags the BSP sets for it, applied the same way esp_lcd_touch
     * applied them - subtracting from the resolution, not from one less than
     * it - so the coordinates land exactly where they used to. */
    x = BSP_LCD_H_RES - x;
    y = BSP_LCD_V_RES - y;

    /* A garbled frame that still had a valid marker would otherwise put the
     * cursor outside the screen, where LVGL's hit testing does odd things. */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= BSP_LCD_H_RES) x = BSP_LCD_H_RES - 1;
    if (y >= BSP_LCD_V_RES) y = BSP_LCD_V_RES - 1;

    *out_x = x;
    *out_y = y;
    return true;
}

static void touch_task(void *arg)
{
    (void)arg;

    uint32_t fail_streak   = 0;
    bool     reported      = false;
    int64_t  last_reset_us = 0;

    while (1) {
        bool    pressed = false;
        int32_t x = 0, y = 0;
        uint8_t frame[TOUCH_FRAME_BYTES];

        esp_err_t err = touch_read_frame(frame);
        if (err == ESP_OK) {
            if (reported) {
                reported = false;
                ESP_LOGI(TAG, "Touch recovered");
            }
            fail_streak = 0;
            pressed = touch_decode(frame, &x, &y);
        } else {
            /* A failed read is not news. With no interrupt line the chip is
             * polled whether or not anyone is touching it, and an idle CST9217
             * answers some of those with a NAK or a frame with no marker in
             * it. Only a long run of them means it has stopped answering. */
            fail_streak++;
            if (fail_streak >= NAV_TOUCH_STUCK_READS) {
                if (!reported) {
                    reported = true;
                    ESP_LOGW(TAG, "Touch stopped answering (%s); the screen still navigates",
                             esp_err_to_name(err));
                }
                int64_t now = esp_timer_get_time();
                if (now - last_reset_us > NAV_TOUCH_RECOVER_MS * 1000LL) {
                    last_reset_us = now;
                    /* Both halves of the only recovery available: the chip,
                     * and the bus it may be holding down. */
                    touch_reset_pulse();
                    i2c_master_bus_reset(bsp_i2c_get_handle());
                }
            }
        }

        portENTER_CRITICAL(&s_touch_mux);
        /* Keep the last coordinate on release: LVGL reads the point that came
         * with it, so zeroing here would land every tap in the corner. */
        if (pressed) {
            s_touch_state.x = x;
            s_touch_state.y = y;
        }
        s_touch_state.pressed = pressed;
        s_touch_state.seq++;
        portEXIT_CRITICAL(&s_touch_mux);

        vTaskDelay(pdMS_TO_TICKS(NAV_TOUCH_POLL_MS));
    }
}

/** Copy the poller's last word. No I2C, no blocking, no error path. */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    static uint32_t last_seq;
    static int64_t  last_change_us;
    static bool     stall_reported;

    portENTER_CRITICAL(&s_touch_mux);
    int32_t  x       = s_touch_state.x;
    int32_t  y       = s_touch_state.y;
    bool     pressed = s_touch_state.pressed;
    uint32_t seq     = s_touch_state.seq;
    portEXIT_CRITICAL(&s_touch_mux);

    data->point.x = x;
    data->point.y = y;
    data->state   = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

    /* Nothing in the poller waits without a deadline any more, so this should
     * stay quiet. It is what found the original hang, so it stays. */
    int64_t now = esp_timer_get_time();
    if (seq != last_seq || last_change_us == 0) {
        last_seq       = seq;
        last_change_us = now;
        if (stall_reported) {
            stall_reported = false;
            ESP_LOGI(TAG, "Touch poller is running again");
        }
    } else if (!stall_reported && now - last_change_us > NAV_TOUCH_STALL_MS * 1000LL) {
        stall_reported = true;
        ESP_LOGW(TAG, "Touch poller has not reported for %d ms - the bus is stuck. "
                      "Navigation carries on without touch.", NAV_TOUCH_STALL_MS);
    }
}

/** Claim the controller and give LVGL an input device that reads memory. */
static bool touch_start(lv_display_t *disp)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus is not up");
        return false;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TOUCH_I2C_ADDR,
        .scl_speed_hz    = CONFIG_BSP_I2C_CLK_SPEED_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_touch_dev) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot address the touch controller");
        return false;
    }

    const gpio_config_t rst_cfg = {
        .pin_bit_mask = BIT64(BSP_LCD_TOUCH_RST),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);

#if NAV_TOUCH_RESET_AFTER_INIT
    /* Start from a known state whatever the last boot left behind. */
    touch_reset_pulse();
#endif

    /* Below the LVGL task, which must never wait on this one. */
    if (xTaskCreate(touch_task, "nav_touch", 4096, NULL, 3, &s_touch_task) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the touch task");
        return false;
    }

    lv_indev_t *indev = lv_indev_create();
    if (!indev) {
        ESP_LOGE(TAG, "Cannot create the input device");
        return false;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, disp);
    return true;
}

/**
 * The SH8601 only accepts windows that start and end on an even column, so
 * every invalidated area is grown to the nearest one. Same arithmetic as the
 * BSP's own rounder, which the path below replaces.
 */
static void display_rounder_cb(lv_area_t *area)
{
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

/**
 * Bring the panel up.
 *
 * Not through bsp_display_start_with_config(), and the reason is worth
 * knowing. That function gives this QSPI panel to lvgl_port_add_disp_rgb(),
 * which is the entry point for panels driven by the S3's RGB LCD peripheral.
 * Two things follow from it.
 *
 * It marks the display as an RGB one, and the port's flush callback answers
 * that by calling lv_disp_flush_ready() the moment esp_lcd_panel_draw_bitmap()
 * returns - while the QSPI DMA is still reading the buffer LVGL is now free to
 * draw the next slice into.
 *
 * And it registers an RGB vsync callback on the handle:
 *
 *     esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &vsync_cbs, ctx)
 *
 * which takes what it is given to be an esp_rgb_panel_t and copies the
 * callbacks into a field several hundred bytes along - past the end of the
 * small SH8601 panel that was actually allocated, into whatever the heap put
 * next to it. There is no type check to stop it. A stray write of that kind
 * does not announce itself; it surfaces later as a hang with nothing in the
 * log, in a part of the program that has nothing to do with the display.
 *
 * So the panel is created with bsp_display_new() - the BSP's own function,
 * which resets, initialises and powers it on, and keeps the handles its
 * brightness control needs - and handed to lvgl_port_add_disp(), which is what
 * every other SPI panel uses. The flags are the ones the BSP asked for; the
 * difference is that the flush now waits for the transfer to finish, because
 * the port takes its completion from the panel IO's own callback.
 */
static lv_display_t *display_start(void)
{
    lv_display_t *disp = NULL;

#if NAV_BSP_DISPLAY_START
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = BSP_LCD_H_RES * CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT,
        .double_buffer = false,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = false,
        },
    };
    cfg.lvgl_port_cfg.task_stack = 8192;
    disp = bsp_display_start_with_config(&cfg);
#else
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* Drawing a map is deeper than drawing the widgets the BSP was sized for. */
    port_cfg.task_stack = 8192;
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL port did not start");
        return NULL;
    }

    bsp_display_config_t hw_cfg = { 0 };
    esp_lcd_panel_handle_t    panel = NULL;
    esp_lcd_panel_io_handle_t io    = NULL;
    if (bsp_display_new(&hw_cfg, &panel, &io) != ESP_OK) {
        ESP_LOGE(TAG, "Panel did not initialise");
        return NULL;
    }

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = BSP_LCD_H_RES * CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT,
        .double_buffer = false,
        .hres          = BSP_LCD_H_RES,
        .vres          = BSP_LCD_V_RES,
        .monochrome    = false,
        .rotation      = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .rounder_cb    = display_rounder_cb,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = false,
            .sw_rotate   = true,
            .swap_bytes  = true,
        },
    };
    disp = lvgl_port_add_disp(&disp_cfg);
#endif

    if (!disp) return NULL;

#if NAV_DISPLAY_ROTATION == 90
    bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90);
#elif NAV_DISPLAY_ROTATION == 180
    bsp_display_rotate(disp, LV_DISPLAY_ROTATION_180);
#elif NAV_DISPLAY_ROTATION == 270
    bsp_display_rotate(disp, LV_DISPLAY_ROTATION_270);
#elif NAV_DISPLAY_ROTATION != 0
#error "NAV_DISPLAY_ROTATION must be 0, 90, 180 or 270"
#endif

    bsp_display_brightness_set(NAV_DISPLAY_BRIGHTNESS);

    /* Swapping the input device touches LVGL state, so hold its lock. */
    bsp_display_lock(0);
    bool touch_ok = touch_start(disp);
    bsp_display_unlock();
    ESP_LOGI(TAG, "Touch %s", touch_ok ? "ready" : "unavailable");

    return disp;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(bsp_i2c_init());

    /* The PMU shares the I2C bus with the touch controller, so it has to come
     * up after bsp_i2c_init(). A board with no cell in it still navigates. */
    nav_power_init();

    /* Started once for the life of the board, not per route, so a route change
     * does not lose the heading or the gyroscope's learned bias. The IMU is on
     * the same bus, so this too comes after bsp_i2c_init(). */
    compass_source_start();

    bool sd_ok = (bsp_sdcard_mount() == ESP_OK);
    if (!sd_ok) {
        ESP_LOGE(TAG, "SD card not mounted; there are no tiles to draw");
    }

    if (display_start() == NULL) {
        ESP_LOGE(TAG, "Display did not start");
        return;
    }
    ESP_LOGI(TAG, "Display up: %dx%d, rotation %d, brightness %d%%",
             BSP_LCD_H_RES, BSP_LCD_V_RES, NAV_DISPLAY_ROTATION,
             NAV_DISPLAY_BRIGHTNESS);

    s_fix_lock = xSemaphoreCreateMutex();
    if (!s_fix_lock) {
        ESP_LOGE(TAG, "Cannot create the fix mutex");
        return;
    }

    bsp_display_lock(0);

    if (!sd_ok) {
        show_fatal("No SD card",
                   "Insert a card holding a tile folder, and routes/ if you "
                   "want them, then reset the board.");
        bsp_display_unlock();
        return;
    }

    /* Routes first, then every tile folder as a map on its own - the one a
     * tiles-only build unpacks, and the ones the routes use. */
    int routes = nav_routes_scan(s_entries, NAV_MAX_ROUTES);
    int maps   = nav_maps_scan(s_entries + routes, NAV_MAX_ROUTES - routes);
    s_entry_count = routes + maps;

    if (s_entry_count == 0) {
        show_fatal("Nothing on the card",
                   "Put a tile folder on the card, and routes in " NAV_ROUTES_DIR
                   " if you want them, then reset the board.");
        bsp_display_unlock();
        return;
    }

    if (routes == 1 || (routes == 0 && maps == 1)) {
        /* Nothing to choose between, so do not make the rider choose. The
         * route's own tiles are also listed as a map, but that is a way to
         * look around, not a second destination; the list button reaches it. */
        start_navigation(&s_entries[0]);
    } else {
        nav_routes_show_picker(s_entries, s_entry_count, on_route_picked, NULL);
    }

    bsp_display_unlock();

    nav_power_start_monitor(NAV_BATTERY_POLL_MS);
    heartbeat_start();

    ESP_LOGI(TAG, "Ready");
}
