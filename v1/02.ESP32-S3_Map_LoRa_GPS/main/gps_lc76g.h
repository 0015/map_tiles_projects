/* LC76G GPS module interface */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif


#define GPS_UART_NUM            UART_NUM_2
#define GPS_UART_TX_PIN         (5)
#define GPS_UART_RX_PIN         (3)
#define GPS_UART_BAUD_RATE      (115200)
#define GPS_UART_BUF_SIZE       (2048)

#define GPS_MAX_NMEA_LEN        (256)
#define GPS_MAX_SATELLITES      (47)
#define GPS_UPDATE_RATE_DEFAULT (1)
#define GPS_UPDATE_RATE_MAX     (10)


typedef enum {
    GPS_FIX_NONE = 0,
    GPS_FIX_2D = 2,
    GPS_FIX_3D = 3,
} gps_fix_type_t;


typedef enum {
    GPS_QUALITY_INVALID = 0,
    GPS_QUALITY_GPS = 1,
    GPS_QUALITY_DGPS = 2,
    GPS_QUALITY_PPS = 3,
    GPS_QUALITY_RTK = 4,
    GPS_QUALITY_FLOAT_RTK = 5,
    GPS_QUALITY_ESTIMATED = 6,
    GPS_QUALITY_MANUAL = 7,
    GPS_QUALITY_SIMULATION = 8,
} gps_quality_t;


typedef struct {
    uint8_t prn;
    uint8_t elevation;
    uint16_t azimuth;
    uint8_t snr;
    bool in_use;
} gps_satellite_t;


typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint16_t millisecond;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} gps_datetime_t;


typedef struct {
    double latitude;
    double longitude;
    float altitude;
    float speed_knots;
    float speed_kmh;
    float course;
    gps_fix_type_t fix_type;
    gps_quality_t fix_quality;
    uint8_t satellites_used;
    uint8_t satellites_visible;
    float hdop;
    float vdop;
    float pdop;
    gps_datetime_t datetime;
    bool valid;
    uint32_t last_update_ms;
} gps_data_t;


typedef struct {
    uint8_t uart_num;
    int tx_pin;
    int rx_pin;
    uint32_t baud_rate;
    uint8_t update_rate_hz;
} gps_config_t;


typedef void (*gps_data_callback_t)(const gps_data_t *data, void *user_data);
typedef void (*gps_nmea_callback_t)(const char *sentence, void *user_data);


esp_err_t gps_init(void);
esp_err_t gps_init_with_config(const gps_config_t *config);
esp_err_t gps_deinit(void);
esp_err_t gps_get_data(gps_data_t *data);
esp_err_t gps_get_satellites(gps_satellite_t *satellites, uint8_t max_satellites, uint8_t *count);
esp_err_t gps_register_data_callback(gps_data_callback_t callback, void *user_data);
esp_err_t gps_register_nmea_callback(gps_nmea_callback_t callback, void *user_data);
esp_err_t gps_set_update_rate(uint8_t rate_hz);

esp_err_t gps_configure_constellations(bool gps_enable, bool glonass_enable, 
                                       bool galileo_enable, bool beidou_enable, 
                                       bool qzss_enable);
esp_err_t gps_hot_start(void);
esp_err_t gps_warm_start(void);
esp_err_t gps_cold_start(void);
esp_err_t gps_factory_reset(void);
esp_err_t gps_set_baud_rate(uint32_t baud_rate);
bool gps_has_fix(void);

uint32_t gps_get_time_since_update(void);
gps_fix_type_t gps_get_fix_type(void);
uint8_t gps_get_satellites_in_use(void);
void gps_format_coordinate(double degrees, bool is_latitude, char *buffer, size_t buffer_len);
float gps_calculate_distance(double lat1, double lon1, double lat2, double lon2);
float gps_calculate_bearing(double lat1, double lon1, double lat2, double lon2);

#ifdef __cplusplus
}
#endif
