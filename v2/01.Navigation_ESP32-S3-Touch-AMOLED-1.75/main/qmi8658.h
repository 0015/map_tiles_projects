/**
 * @file qmi8658.h
 * @brief Just enough of the QST QMI8658 to keep track of a heading.
 *
 * A six-axis part: an accelerometer and a gyroscope, and no magnetometer. It
 * says how fast the board is turning and which way is down, never which way
 * is north - compass_source.c gets that from the GPS and uses this chip to
 * carry it between fixes.
 *
 * Built on the ESP-IDF i2c_master driver, so it shares the BSP's bus with the
 * touch controller and the PMU; the driver serialises the three. The vendor
 * demo reads it through SensorLib and the legacy I2C driver, which ESP-IDF 5
 * refuses to run next to the new one that everything else here uses.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QMI8658_ADDR_HIGH   0x6B    /**< SA0 high - how this board straps it */
#define QMI8658_ADDR_LOW    0x6A

typedef struct qmi8658_dev_t *qmi8658_handle_t;

/** @brief One reading of both sensors, in the chip's own axes. */
typedef struct {
    float acc_g[3];     /**< Specific force in g: about +1 along whichever axis points up at rest */
    float gyr_dps[3];   /**< Angular rate in degrees per second, right-handed about each axis */
} qmi8658_sample_t;

/**
 * @brief Find the chip, reset it, and start both sensors.
 *
 * Configured for ±4 g and ±512 dps at 112 Hz, low-passed on the chip to about
 * 15 Hz so that a host reading it at 50 Hz sees nothing it could alias.
 *
 * @return ESP_ERR_NOT_FOUND when nothing answers at the address, and
 *         ESP_ERR_INVALID_RESPONSE when something does but it is not a QMI8658.
 */
esp_err_t qmi8658_init(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t scl_speed_hz,
                       qmi8658_handle_t *out);

void qmi8658_deinit(qmi8658_handle_t dev);

/** @brief The latest sample of both sensors, in one bus transaction. */
esp_err_t qmi8658_read(qmi8658_handle_t dev, qmi8658_sample_t *out);

#ifdef __cplusplus
}
#endif
