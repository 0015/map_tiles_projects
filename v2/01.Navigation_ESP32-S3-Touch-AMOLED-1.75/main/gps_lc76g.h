#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************************
 * LC76G GNSS Module Driver for ESP32-S3
 * 
 * Hardware: Waveshare LC76G GNSS Module
 * UART: UART2 (configurable; the ESP32-S3 has UART0-2)
 * TX: GPIO (to be configured)
 * RX: GPIO (to be configured)
 * Protocol: NMEA 0183
 * Default Baud Rate: 115200 (can be changed via PAIR864 command)
 * Support: GPS, GLONASS, Galileo, BeiDou, QZSS
 * Chip: AG3352Q
 * Update Rate: 1-10Hz (default 1Hz)
 **************************************************************************************************/

/* UART Configuration - Default values, can be changed before init */
#define GPS_UART_NUM            UART_NUM_2
#define GPS_UART_TX_PIN         (17)          // ESP32-S3 -> module RX
#define GPS_UART_RX_PIN         (18)          // ESP32-S3 <- module TX
#define GPS_UART_BAUD_RATE      (115200)      // LC76G default baud rate
#define GPS_UART_BUF_SIZE       (2048)

/* GPS Configuration */
#define GPS_MAX_NMEA_LEN        (256)        // Maximum NMEA sentence length
#define GPS_MAX_SATELLITES      (47)         // LC76G supports 47 tracking channels
#define GPS_UPDATE_RATE_DEFAULT (1)          // Default 1Hz update rate
#define GPS_UPDATE_RATE_MAX     (10)         // Maximum 10Hz update rate

/**
 * @brief GPS fix status
 */
typedef enum {
    GPS_FIX_NONE = 0,           // No fix
    GPS_FIX_2D = 2,             // 2D fix
    GPS_FIX_3D = 3,             // 3D fix
} gps_fix_type_t;

/**
 * @brief GPS fix quality indicator (from GGA)
 */
typedef enum {
    GPS_QUALITY_INVALID = 0,    // Invalid
    GPS_QUALITY_GPS = 1,        // GPS fix (SPS)
    GPS_QUALITY_DGPS = 2,       // DGPS fix
    GPS_QUALITY_PPS = 3,        // PPS fix
    GPS_QUALITY_RTK = 4,        // Real Time Kinematic
    GPS_QUALITY_FLOAT_RTK = 5,  // Float RTK
    GPS_QUALITY_ESTIMATED = 6,  // Estimated (dead reckoning)
    GPS_QUALITY_MANUAL = 7,     // Manual input mode
    GPS_QUALITY_SIMULATION = 8, // Simulation mode
} gps_quality_t;

/**
 * @brief GPS satellite information
 */
typedef struct {
    uint8_t prn;                // Satellite PRN number
    uint8_t elevation;          // Elevation in degrees (0-90)
    uint16_t azimuth;           // Azimuth in degrees (0-359)
    uint8_t snr;                // Signal to noise ratio in dB (0-99)
    bool in_use;                // Satellite used in fix
} gps_satellite_t;

/**
 * @brief GPS date and time
 */
typedef struct {
    uint8_t hour;               // Hour (0-23)
    uint8_t minute;             // Minute (0-59)
    uint8_t second;             // Second (0-59)
    uint16_t millisecond;       // Millisecond (0-999)
    uint8_t day;                // Day (1-31)
    uint8_t month;              // Month (1-12)
    uint16_t year;              // Year (e.g., 2025)
} gps_datetime_t;

/**
 * @brief GPS position data
 */
typedef struct {
    // Position
    double latitude;            // Latitude in degrees (-90 to +90)
    double longitude;           // Longitude in degrees (-180 to +180)
    float altitude;             // Altitude above sea level in meters
    
    // Velocity
    float speed_knots;          // Speed over ground in knots
    float speed_kmh;            // Speed over ground in km/h
    float course;               // Course over ground in degrees (0-359.99)
    
    // Accuracy and fix information
    gps_fix_type_t fix_type;    // Fix type (2D/3D)
    gps_quality_t fix_quality;  // Fix quality indicator
    uint8_t satellites_used;    // Number of satellites used in fix
    uint8_t satellites_visible; // Number of satellites visible
    float hdop;                 // Horizontal dilution of precision
    float vdop;                 // Vertical dilution of precision
    float pdop;                 // Position dilution of precision
    
    // Time
    gps_datetime_t datetime;    // UTC date and time
    
    // Status
    bool valid;                 // Position data valid
    uint32_t last_update_ms;    // Last update timestamp (ms since boot)
} gps_data_t;

/**
 * @brief GPS configuration structure
 */
typedef struct {
    uint8_t uart_num;           // UART port number
    int tx_pin;                 // TX pin GPIO number
    int rx_pin;                 // RX pin GPIO number
    uint32_t baud_rate;         // UART baud rate (default 115200)
    uint8_t update_rate_hz;     // GPS update rate in Hz (1-10)
} gps_config_t;

/**
 * @brief GPS data callback function type
 * 
 * @param data Pointer to GPS data structure
 * @param user_data User data passed during callback registration
 */
typedef void (*gps_data_callback_t)(const gps_data_t *data, void *user_data);

/**
 * @brief GPS NMEA sentence callback function type
 * 
 * @param sentence NMEA sentence string
 * @param user_data User data passed during callback registration
 */
typedef void (*gps_nmea_callback_t)(const char *sentence, void *user_data);

/**
 * @brief Initialize GPS module with default configuration
 * 
 * Default: 115200 baud, 1Hz update rate
 * Note: You must set GPS_UART_TX_PIN and GPS_UART_RX_PIN before calling this
 * 
 * @return
 *      - ESP_OK: Success
 *      - ESP_FAIL: Failed to initialize
 */
esp_err_t gps_init(void);

/**
 * @brief Initialize GPS module with custom configuration
 * 
 * @param config Pointer to configuration structure
 * @return
 *      - ESP_OK: Success
 *      - ESP_FAIL: Failed to initialize
 */
esp_err_t gps_init_with_config(const gps_config_t *config);

/**
 * @brief Deinitialize GPS module
 * 
 * @return
 *      - ESP_OK: Success
 */
esp_err_t gps_deinit(void);

/**
 * @brief Get current GPS position data
 * 
 * @param data Pointer to GPS data structure to fill
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 *      - ESP_ERR_INVALID_STATE: GPS not initialized
 */
esp_err_t gps_get_data(gps_data_t *data);

/**
 * @brief Get satellite information
 * 
 * @param satellites Array to store satellite information
 * @param max_satellites Maximum number of satellites to retrieve
 * @param count Pointer to store actual number of satellites retrieved
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid argument
 *      - ESP_ERR_INVALID_STATE: GPS not initialized
 */
esp_err_t gps_get_satellites(gps_satellite_t *satellites, uint8_t max_satellites, uint8_t *count);

/**
 * @brief Register callback for GPS data updates
 * 
 * Called whenever new valid position data is received
 * 
 * @param callback Callback function
 * @param user_data User data to pass to callback
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid callback
 */
esp_err_t gps_register_data_callback(gps_data_callback_t callback, void *user_data);

/**
 * @brief Register callback for raw NMEA sentences
 * 
 * Called for each complete NMEA sentence received
 * 
 * @param callback Callback function
 * @param user_data User data to pass to callback
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid callback
 */
esp_err_t gps_register_nmea_callback(gps_nmea_callback_t callback, void *user_data);

/**
 * @brief Set GPS update rate (using PAIR050 command)
 * 
 * @param rate_hz Update rate in Hz (1-10)
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid rate
 *      - ESP_ERR_INVALID_STATE: GPS not initialized
 */
esp_err_t gps_set_update_rate(uint8_t rate_hz);

/**
 * @brief Configure GNSS constellations (using PAIR066 command)
 * 
 * @param gps_enable Enable GPS
 * @param glonass_enable Enable GLONASS
 * @param galileo_enable Enable Galileo
 * @param beidou_enable Enable BeiDou
 * @param qzss_enable Enable QZSS
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_STATE: GPS not initialized
 */
esp_err_t gps_configure_constellations(bool gps_enable, bool glonass_enable, 
                                       bool galileo_enable, bool beidou_enable, 
                                       bool qzss_enable);

/**
 * @brief Perform hot start (using PAIR004 command)
 * 
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_STATE: GPS not initialized
 */
esp_err_t gps_hot_start(void);

/**
 * @brief Perform warm start (using PAIR005 command)
 * 
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_STATE: GPS not initialized
 */
esp_err_t gps_warm_start(void);

/**
 * @brief Perform cold start (using PAIR006 command)
 * 
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_STATE: GPS not initialized
 */
esp_err_t gps_cold_start(void);

/**
 * @brief Clear system configuration and perform cold start (using PAIR007 command)
 * 
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_STATE: GPS not initialized
 */
esp_err_t gps_factory_reset(void);

/**
 * @brief Change UART baud rate (using PAIR864 command)
 * 
 * Supported baud rates: 9600, 115200 (default), 230400, 460800, 921600, 3000000
 * 
 * @param baud_rate New baud rate
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Unsupported baud rate
 *      - ESP_ERR_INVALID_STATE: GPS not initialized
 */
esp_err_t gps_set_baud_rate(uint32_t baud_rate);

/**
 * @brief Check if GPS has a valid fix
 * 
 * @return true if GPS has valid fix, false otherwise
 */
bool gps_has_fix(void);

/**
 * @brief Get time since last GPS update
 * 
 * @return Milliseconds since last update, or UINT32_MAX if never updated
 */
uint32_t gps_get_time_since_update(void);

#ifdef __cplusplus
}
#endif
