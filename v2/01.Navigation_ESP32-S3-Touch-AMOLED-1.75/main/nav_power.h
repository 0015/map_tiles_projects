/**
 * @file nav_power.h
 * @brief Battery and charger state, read from the board's AXP2101.
 *
 * This board runs off a cell, and on a device you take outside "how much is
 * left" belongs on screen next to the satellite count.
 *
 * Reading the PMU is a handful of I2C transactions, so it happens in its own
 * low-priority task and the UI reads the last snapshot. Nothing here touches
 * LVGL, and ::nav_power_get is safe to call from the LVGL thread.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool present;       /**< The PMU answered at start-up */
    bool battery;       /**< A cell is connected */
    bool charging;
    bool vbus;          /**< USB power present */
    int  percent;       /**< 0-100, or -1 when the gauge has no reading yet */
} nav_power_state_t;

/**
 * @brief Bring up the PMU.
 *
 * The I2C bus must already be initialised (::bsp_i2c_init).
 *
 * @return false if the AXP2101 did not answer. The app carries on without a
 *         battery readout rather than refusing to navigate.
 */
bool nav_power_init(void);

/** @brief Start polling in the background. Does nothing if the PMU is absent. */
void nav_power_start_monitor(uint32_t period_ms);

void nav_power_stop_monitor(void);

/** @brief The latest snapshot. Always fills @p out, even with no PMU. */
void nav_power_get(nav_power_state_t *out);

#ifdef __cplusplus
}
#endif
