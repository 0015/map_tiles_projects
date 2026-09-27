/**
 * @file nav_config.h
 * @brief Everything you are likely to want to change lives here.
 *
 * This is the ESP32-S3 / 1.75 inch build. The guidance logic knows nothing
 * about the board; what is set here for it is the wiring, the panel size, and
 * how much of the screen a finger covers.
 */

#pragma once

#include "driver/uart.h"

/* ------------------------------------------------------------ position source
 *
 * NAV_GPS_SOURCE_LC76G      read the LC76G module over UART (needs sky view)
 * NAV_GPS_SOURCE_SIMULATOR  drive along the route at a fixed speed, so the whole
 *                           guidance path can be exercised on a desk
 */
#define NAV_GPS_SOURCE_LC76G        0
#define NAV_GPS_SOURCE_SIMULATOR    1

/* Swap these two lines to demo the whole app with no receiver wired up. */
#ifndef NAV_GPS_SOURCE
//#define NAV_GPS_SOURCE              NAV_GPS_SOURCE_SIMULATOR
#define NAV_GPS_SOURCE              NAV_GPS_SOURCE_LC76G
#endif

/* LC76G wiring.
 *
 * The ESP32-S3 has three UARTs and UART0 is the console, so this is UART2.
 * GPIO 17 and 18 are the two the board brings out and the vendor
 * demo already used: the BSP claims 14/15 for I2C, 8-10 and 42/45/46 for I2S,
 * 4-7, 12 and 38/39 for the panel, 40 for the touch reset and 1-3 for the SD
 * card, and octal PSRAM takes 33-37. */
#define NAV_GPS_UART_NUM            UART_NUM_2
#define NAV_GPS_UART_TX_PIN         17      /**< ESP32-S3 -> module RX */
#define NAV_GPS_UART_RX_PIN         18      /**< ESP32-S3 <- module TX */
#define NAV_GPS_UART_BAUD           115200
#define NAV_GPS_UPDATE_HZ           5

/* Simulator behaviour */
#define NAV_SIM_SPEED_KMH           45.0    /**< Cruising speed at 1x */
#define NAV_SIM_UPDATE_HZ           10
#define NAV_SIM_NOISE_M             1.5     /**< Lateral jitter, to prove the matcher copes */
#define NAV_SIM_START_DELAY_MS      2000    /**< Show the "acquiring" state first */
#define NAV_SIM_SATELLITES          12      /**< Reported in the status chip */

/* ------------------------------------------------------------- demo mode
 *
 * On-screen transport controls for the simulator: play/pause, a speed
 * multiplier, skip to the next turn, stray off the road, and restart. They
 * appear with the rest of the controls when you tap the map.
 */
#define NAV_DEMO_CONTROLS           1       /**< 0 hides the bar entirely */
#define NAV_DEMO_SPEED_STEPS        { 1.0f, 2.0f, 4.0f, 8.0f }
#define NAV_DEMO_SKIP_LEAD_M        250.0   /**< Land this far before the next turn */

/** How fast the demo's stray button pulls the rider away from the road, and
 *  back again. Fast enough to see the alerts escalate without waiting. */
#define NAV_DEMO_STRAY_MPS          9.0

/* ------------------------------------------------------------------- storage */
#define NAV_SD_MOUNT_POINT          "/sdcard"
#define NAV_TILE_FOLDER             "tiles1"
#define NAV_ROUTES_DIR              NAV_SD_MOUNT_POINT "/routes"

/** Routes and tile folders listed together, routes first. */
#define NAV_MAX_ROUTES              24

/** Fallback for cards written before routes lived in a folder of their own. */
#define NAV_ROUTE_PATH              NAV_SD_MOUNT_POINT "/route.bin"

/* ------------------------------------------------------------------- display
 *
 * Rotation here is done by LVGL in software - the display is registered with
 * `sw_rotate`, and the ESP32-S3 has no PPA to hand it to. Every rotated
 * frame is an extra pass over 466x466 pixels on the CPU, which this board
 * cannot spare while a map is moving underneath it.
 *
 * The panel comes up the right way round in the stock housing, so leave this
 * at 0 unless yours does not. 180 is as cheap as 0 in the rotation pass; 90 and
 * 270 also transpose, which is the slow case.
 */
#define NAV_DISPLAY_ROTATION        0       /**< 0, 90, 180 or 270 */

/** AMOLED brightness, 0-100. There is no backlight pin: this is panel command
 *  0x51, so it costs nothing per frame. */
#define NAV_DISPLAY_BRIGHTNESS      100

/**
 * Bring the panel up through bsp_display_start_with_config() instead of doing
 * it here.
 *
 * Left at 0, and the reason is in main.c: the BSP hands this QSPI panel to the
 * LVGL port's *RGB* entry point, which writes an RGB panel's callbacks into a
 * handle that is not one. Set this to 1 to go back to the vendor path - the
 * app works either way, but that write lands in somebody else's heap.
 */
#define NAV_BSP_DISPLAY_START       0

/* --------------------------------------------------------------------- touch
 *
 * The CST9217 is read from main.c with the ordinary I2C master API, not
 * through esp_lcd_touch and the LVGL port. That is the answer to a hang, not a
 * preference.
 *
 * esp_lcd's I2C transport passes -1 as its transaction timeout, which means
 * wait forever, and the vendor stack makes that call from inside the LVGL
 * thread. When the bus wedges - and on this board it does - the user interface
 * goes with it, with no panic and no watchdog to explain it, because the task
 * is blocked rather than spinning. Measured here, once the poller had been
 * moved to a task of its own so the UI survived to report it:
 *
 *     W nav: Touch poller has not reported for 3000 ms - the bus is stuck.
 *
 * The register and the frame format below are the vendor driver's. What is
 * different is that every transfer has a deadline, so a stuck bus returns an
 * error instead of swallowing the caller - and that nothing ever puts the
 * controller into the command mode the vendor driver left it in.
 */

/** Ceiling on any single transfer to the digitiser. A frame is ten bytes at
 *  400 kHz, so under a millisecond; this is pure insurance. */
#define NAV_TOUCH_I2C_TIMEOUT_MS    50

/** Pulse the touch reset line at start-up. Set to 0 if the panel blanks: the
 *  BSP header hints the reset may be shared with the display's. */
#define NAV_TOUCH_RESET_AFTER_INIT  1

/**
 * How often the digitiser is polled, by the task that owns it.
 *
 * There is no interrupt line wired on this board, so every one of these is an
 * I2C conversation with a chip that is usually idle. 30 ms matches the rate
 * the map itself moves at. LVGL reads the result from memory, at its own
 * refresh rate, and never touches the bus.
 */
#define NAV_TOUCH_POLL_MS           30

/**
 * How long the poller may go quiet before the UI says so.
 *
 * With a deadline on every transfer this should no longer be reachable. It is
 * kept because it is the one thing that located the freeze, and because a
 * driver that cannot be starved is worth proving rather than assuming.
 */
#define NAV_TOUCH_STALL_MS          3000

/**
 * Failed reads in a row before the controller is considered stuck.
 *
 * An idle CST9217 NAKs an access now and then, which means nothing. Only a run
 * this long says it has stopped answering, and earns a reset pulse and an I2C
 * bus recovery.
 */
#define NAV_TOUCH_STUCK_READS       100

/** Never pulse the reset, or reset the bus, more often than this. */
#define NAV_TOUCH_RECOVER_MS        10000

/* ------------------------------------------------------------------ map view */
#define NAV_ZOOM_DEFAULT            17
#define NAV_ZOOM_MIN                12
#define NAV_ZOOM_MAX                19

/**
 * Tile slots, 128 KB each.
 *
 * A 466x466 view shows at most 3x3 tiles, and map_view only warms the ring
 * around them when the cache can hold the ring as well - 5x5 = 25 slots. 28
 * leaves a little slack for panning, and costs 3.5 MB of the 8 MB PSRAM.
 */
#define NAV_TILE_CACHE_SLOTS        28

/**
 * How far below the screen centre the vehicle sits.
 *
 * 0 puts you in the middle of the band of map left between the turn banner and
 * the summary panel, which on a circle this small is where you want it. There
 * is no room to trade view behind you for road ahead here the way a car head
 * unit does.
 */
#define NAV_FOLLOW_OFFSET_PX        0

/* -------------------------------------------------------------- controls
 *
 * At 466 px across, a comfortable touch target is about a seventh of the
 * screen, so four of them permanently on the glass would cover the map. They
 * hide instead, and a tap anywhere on the map brings them back.
 *
 * The exception is re-centring: once follow mode has been released by a drag,
 * that button stays up on its own until it is used, because it is the only way
 * back and nothing else on screen says so.
 */
#define NAV_CONTROLS_AUTOHIDE_MS    6000

/* ------------------------------------------------------------------ battery
 *
 * The board runs off a cell through an AXP2101, so it can say how much charge
 * is left. Reading it costs a handful of I2C transactions,
 * which is why it happens in its own task rather than in the draw path.
 */
#define NAV_BATTERY_POLL_MS         10000
#define NAV_BATTERY_LOW_PCT         20      /**< Below this the readout turns red */

/* ------------------------------------------------------------------- compass
 *
 * The board's QMI8658 drives a compass dial on the map. The map stays
 * north-up; the dial shows where north is in the real
 * world relative to the top of the screen, so the rider can turn until the
 * needle points up and the map lines up with the road in front of them.
 *
 * The QMI8658 has no magnetometer, only an accelerometer and a gyroscope, so
 * it cannot find north by itself. The GPS course sets north whenever the
 * device travels in a straight line, and the gyroscope carries the heading
 * from there: through a stop at a light, a U-turn, the bike being wheeled
 * round. The dial stays hidden until the first course has set it, and fades
 * while the gyroscope has gone too long without one.
 *
 * There is no declination to set: the GPS course is already relative to true
 * north.
 */
#define NAV_COMPASS_ENABLED         1

/** On the BSP's bus, GPIO 15 (SDA) and 14 (SCL), with the touch controller and
 *  the PMU. This board straps it to 0x6B; 0x6A is tried as well. */
#define NAV_COMPASS_I2C_ADDR        0x6B

/** How often the gyroscope is read and the heading moved on. */
#define NAV_COMPASS_SAMPLE_HZ       50

/**
 * Where the top of the screen points relative to the direction of travel,
 * degrees clockwise, as the device is mounted.
 *
 * North is taken from the course over ground, which is the direction of
 * travel, so this is all the compass needs to know about the mounting: 0 for
 * a unit on the handlebars or the dash with the top of the screen facing
 * forward, 90 if it faces right, 180 if it faces the rider. How the chip sits
 * on the board does not matter - the turn rate is measured about gravity,
 * whichever way up the board is.
 */
#define NAV_COMPASS_MOUNT_OFFSET_DEG    0.0

/**
 * The slowest travel whose GPS course is allowed to set north.
 *
 * At walking pace a receiver's course wanders by tens of degrees. That is
 * still good enough to point the marker (NAV_HEADING_MIN_SPEED_MPS) but not
 * to set a compass by. 2 m/s is a jog or the slowest cycling; faster courses
 * are weighted as the better readings they are.
 */
#define NAV_COMPASS_ALIGN_MIN_SPEED_MPS 2.0

/**
 * How far off the heading may have drifted, in degrees, before the dial is
 * drawn faded and the marker stops using it while stopped.
 */
#define NAV_COMPASS_TRUST_DEG       15.0

/** Past this estimated error the heading is given up on, and the dial hides
 *  until a GPS course sets north again. */
#define NAV_COMPASS_GIVE_UP_DEG     90.0

/* ------------------------------------------------------------------ guidance */
#define NAV_OFF_ROUTE_TOLERANCE_M   35.0    /**< Cross-track error that counts as "off" */
#define NAV_OFF_ROUTE_STRIKES       3       /**< Consecutive bad fixes before we say so */
#define NAV_ON_ROUTE_STRIKES        2       /**< Consecutive good fixes to recover */
#define NAV_ARRIVAL_RADIUS_M        25.0

/* ---------------------------------------------------------- getting back
 *
 * Off route, the rider needs three things and in this order: that they are
 * off, how far and which way the road is, and something on screen that points
 * at it. The first is a one-off alert; the last two have to keep updating,
 * because the useful question changes from "what happened" to "am I getting
 * closer" within seconds.
 */

/** Cross-track beyond which a second, firmer alert is raised. */
#define NAV_OFF_ROUTE_ESCALATE_M    150.0

/** Say it again if the rider is still off after this long. Silence for minutes
 *  reads as the device having given up. */
#define NAV_OFF_ROUTE_REPEAT_S      45

/** Below this rate of change the rider is neither closing nor drifting, so the
 *  UI says nothing about the trend rather than flickering between the two. */
#define NAV_OFF_ROUTE_TREND_MPS     0.6

/** Distances at which the next turn is announced, in metres. */
#define NAV_ANNOUNCE_FAR_M          400
#define NAV_ANNOUNCE_NEAR_M         150
#define NAV_ANNOUNCE_NOW_M          40

/** Speed below which the ETA falls back to the route's planned average. */
#define NAV_ETA_MIN_SPEED_MPS       1.5

/* ------------------------------------------------------------------ map only
 *
 * A tile folder opened on its own, with no route: the map, your position on it,
 * and the outing so far - distance covered, time since the first fix, speed.
 *
 * With no route to measure progress along, distance comes from the receiver's
 * speed, added up over time. Not from the positions: a fix wanders a few
 * metres either way even standing still, and summing the gaps between fixes
 * counts every wobble - 8% too far over a simulated 1 km walk at 5 Hz. The
 * speed is measured from the Doppler shift instead, and has no such wobble.
 */

/** Below this smoothed speed nothing is counted: standing still, or near it. */
#define NAV_FREE_MIN_SPEED_MPS      0.5

/** A longer gap between fixes - a tunnel, heavy tree cover - is bridged with
 *  the straight line between the fixes either side of it instead. */
#define NAV_FREE_MAX_GAP_S          3.0

/**
 * Speed below which course over ground is not believed.
 *
 * NMEA reports the direction of travel, which is undefined when there is none:
 * stopped at a light a receiver emits zero, the last value, or noise. Taking it
 * literally spins the marker and flips "the route is to your left" to "to your
 * right" every second. Below this, the last heading from real movement is held
 * - or, once the compass has a heading it trusts, the way the device faces.
 */
#define NAV_HEADING_MIN_SPEED_MPS   1.0
#define NAV_SPEED_SMOOTHING         0.25    /**< Exponential filter on reported speed */

/* ---------------------------------------------------------------------- units
 *
 * 1  feet and miles, mph:   "850 ft", then tenths of a mile - "0.2 mi", "12.4 mi"
 * 0  metres and kilometres, km/h:  "250 m", then "1.4 km"
 *
 * Every distance and speed on screen follows this - the turn banner, the
 * summary panel, the off-route readout and the route list. The serial log
 * stays metric, and so do the settings in this file.
 */
#define NAV_USE_IMPERIAL            1

/* ----------------------------------------------------------------- debugging */
#define NAV_SHOW_MAP_DEBUG_OVERLAY  0       /**< Cache/segment counters on the map */

/**
 * Seconds between heartbeat lines, or 0 for none.
 *
 * One line, from a task of its own, saying whether the LVGL thread has moved
 * since the last one. A frozen screen with an empty log is otherwise almost
 * impossible to place: this separates "the UI thread is blocked" - on the
 * touch bus, on a flush - from "the whole device is gone", without a debugger
 * and without being at the roadside when it happens.
 */
#define NAV_HEARTBEAT_S             10

/* --------------------------------------------------------------------- fonts
 *
 * Every size here is a step below what a larger panel would carry: 466 px is
 * also a physically small piece of glass, so the text cannot simply scale with
 * the pixels or it stops being readable at a glance. What survived at full size is the one number that matters - the
 * distance to the next turn.
 *
 * The built-in Montserrat faces cover Latin plus the LV_SYMBOL glyphs. They
 * carry no CJK, Cyrillic, Arabic or Devanagari, so a route through a city that
 * names its streets in one of those will show them as empty boxes. To fix that,
 * run LVGL's font converter over a face that has the script you need (subset it
 * to the characters your routes actually use), add the generated .c to this
 * component, declare it with LV_FONT_DECLARE, and point NAV_FONT_STREET at it.
 * Only the street line needs it; the rest of the UI is digits and symbols.
 */
#define NAV_FONT_TURN_SYMBOL    &lv_font_montserrat_28
#define NAV_FONT_TURN_DISTANCE  &lv_font_montserrat_28
#define NAV_FONT_STREET         &lv_font_montserrat_16
#define NAV_FONT_VALUE          &lv_font_montserrat_20
#define NAV_FONT_CAPTION        &lv_font_montserrat_12
#define NAV_FONT_CHIP           &lv_font_montserrat_14
#define NAV_FONT_BUTTON         &lv_font_montserrat_20
