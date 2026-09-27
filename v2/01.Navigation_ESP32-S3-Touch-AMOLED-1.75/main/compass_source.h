/**
 * @file compass_source.h
 * @brief Which way the device is pointing, from the QMI8658 and the GPS course.
 *
 * The QMI8658 on this board has only an accelerometer and a gyroscope, and no
 * magnetometer: it can tell how far the device has turned, never which way it
 * faced to begin with. So north comes from the GPS. Travelling in a straight line, the course
 * over ground is the way the device faces; that sets the heading, and the
 * gyroscope carries it through the moments the GPS has nothing to say - a stop
 * at a light, a U-turn in a car park, the bike being wheeled round.
 *
 * Runs for the life of the board rather than per route, so a route change does
 * not throw away the heading or what has been learned about the gyroscope.
 *
 * Nothing valid is reported until a GPS course has set north, so the dial on
 * the map stays hidden until then rather than point somewhere made up. A
 * sensor that does not answer is looked for again every few seconds.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /**
     * Where the device points, degrees clockwise from true north: the direction
     * of travel when north was last set, plus NAV_COMPASS_MOUNT_OFFSET_DEG,
     * turned by whatever the gyroscope has seen since.
     */
    float    heading_deg;
    /**
     * How far the heading may have wandered since a GPS course last set it,
     * in degrees. It grows while the device turns or is carried about, stays
     * put while it is still, and shrinks again once there is a course.
     */
    float    error_deg;
    /** false with no sensor, before north is first set, once error_deg passes
     *  NAV_COMPASS_GIVE_UP_DEG, or once the sensor stops answering */
    bool     valid;
    uint32_t timestamp_ms;
} nav_compass_t;

/**
 * @brief Start looking for the sensor, and track the heading once found.
 *
 * Call after nvs_flash_init() and bsp_i2c_init(). Does nothing and returns
 * false when NAV_COMPASS_ENABLED is 0.
 */
bool compass_source_start(void);

/**
 * @brief Hand over a GPS report to set north from.
 *
 * Call it with every fix. Only reports of straight-line travel at
 * NAV_COMPASS_ALIGN_MIN_SPEED_MPS or more are used. Safe from any task, and
 * never blocks: the report is copied, and the compass task picks it up.
 *
 * @param course_deg Course over ground, degrees clockwise from true north
 */
void compass_source_feed_course(bool valid, double speed_mps, double course_deg);

/**
 * @brief The latest heading. Safe from any task, and never blocks.
 *
 * @return out->valid. A reading older than a second counts as invalid.
 */
bool compass_source_get(nav_compass_t *out);

#ifdef __cplusplus
}
#endif
