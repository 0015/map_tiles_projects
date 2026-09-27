#include "gps_lc76g.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "GPS_LC76G";

/* Internal state */
static bool gps_initialized = false;
static gps_data_t current_gps_data = {0};
static gps_satellite_t satellites[GPS_MAX_SATELLITES] = {0};
static uint8_t satellite_count = 0;
static QueueHandle_t uart_queue = NULL;
static TaskHandle_t rx_task_handle = NULL;
static gps_data_callback_t data_callback = NULL;
static void *data_callback_user_data = NULL;
static gps_nmea_callback_t nmea_callback = NULL;
static void *nmea_callback_user_data = NULL;

/* UART configuration */
static uint8_t gps_uart_num = GPS_UART_NUM;
static int gps_tx_pin = GPS_UART_TX_PIN;
static int gps_rx_pin = GPS_UART_RX_PIN;

/* Default configuration */
static const gps_config_t default_config = {
    .uart_num = GPS_UART_NUM,
    .tx_pin = GPS_UART_TX_PIN,
    .rx_pin = GPS_UART_RX_PIN,
    .baud_rate = GPS_UART_BAUD_RATE,
    .update_rate_hz = GPS_UPDATE_RATE_DEFAULT,
};

/* Constants for calculations */
#define EARTH_RADIUS_M 6371000.0  // Earth radius in meters
#define DEG_TO_RAD (M_PI / 180.0)
#define RAD_TO_DEG (180.0 / M_PI)

/**
 * @brief Calculate NMEA checksum
 */
static uint8_t nmea_checksum(const char *sentence)
{
    uint8_t checksum = 0;
    const char *p = sentence;
    
    // Skip '$' if present
    if (*p == '$') p++;
    
    // Calculate XOR of all characters until '*'
    while (*p && *p != '*') {
        checksum ^= *p;
        p++;
    }
    
    return checksum;
}

/**
 * @brief Verify NMEA sentence checksum
 */
static bool nmea_verify_checksum(const char *sentence)
{
    const char *p = strchr(sentence, '*');
    if (!p) return false;
    
    uint8_t expected = nmea_checksum(sentence);
    uint8_t actual = (uint8_t)strtol(p + 1, NULL, 16);
    
    return expected == actual;
}

/**
 * @brief Convert NMEA coordinate to decimal degrees
 * Format: DDMM.MMMM or DDDMM.MMMM
 */
static double nmea_to_degrees(const char *coord, char direction)
{
    if (!coord || strlen(coord) < 4) return 0.0;
    
    char *dot = strchr(coord, '.');
    if (!dot) return 0.0;
    
    // Determine if latitude (DDMM.MMMM) or longitude (DDDMM.MMMM)
    int deg_len = (direction == 'N' || direction == 'S') ? 2 : 3;
    
    // Extract degrees
    char deg_str[4] = {0};
    strncpy(deg_str, coord, deg_len);
    double degrees = atof(deg_str);
    
    // Extract minutes
    double minutes = atof(coord + deg_len);
    
    // Convert to decimal degrees
    double result = degrees + (minutes / 60.0);
    
    // Apply direction
    if (direction == 'S' || direction == 'W') {
        result = -result;
    }
    
    return result;
}

/**
 * @brief Parse NMEA time (HHMMSS.SSS)
 */
static void parse_nmea_time(const char *time_str, gps_datetime_t *datetime)
{
    if (!time_str || strlen(time_str) < 6) return;
    
    char buf[3] = {0};
    
    // Hours
    buf[0] = time_str[0];
    buf[1] = time_str[1];
    buf[2] = '\0';
    datetime->hour = atoi(buf);
    
    // Minutes
    buf[0] = time_str[2];
    buf[1] = time_str[3];
    buf[2] = '\0';
    datetime->minute = atoi(buf);
    
    // Seconds
    buf[0] = time_str[4];
    buf[1] = time_str[5];
    buf[2] = '\0';
    datetime->second = atoi(buf);
    
    // Milliseconds (if available)
    const char *dot = strchr(time_str, '.');
    if (dot) {
        datetime->millisecond = (uint16_t)(atof(dot) * 1000);
    }
}

/**
 * @brief Parse NMEA date (DDMMYY)
 */
static void parse_nmea_date(const char *date_str, gps_datetime_t *datetime)
{
    if (!date_str || strlen(date_str) < 6) return;
    
    char buf[3] = {0};
    
    // Day
    buf[0] = date_str[0];
    buf[1] = date_str[1];
    buf[2] = '\0';
    datetime->day = atoi(buf);
    
    // Month
    buf[0] = date_str[2];
    buf[1] = date_str[3];
    buf[2] = '\0';
    datetime->month = atoi(buf);
    
    // Year (YY -> YYYY)
    buf[0] = date_str[4];
    buf[1] = date_str[5];
    buf[2] = '\0';
    datetime->year = 2000 + atoi(buf);
}

/**
 * @brief Get field from NMEA sentence
 */
static bool get_nmea_field(const char *sentence, int field_num, char *buffer, size_t buffer_len)
{
    if (!sentence || !buffer || buffer_len == 0) return false;
    
    const char *p = sentence;
    int current_field = 0;
    
    // Skip to field
    while (*p && current_field < field_num) {
        if (*p == ',') current_field++;
        p++;
    }
    
    if (current_field != field_num) return false;
    
    // Extract field
    size_t i = 0;
    while (*p && *p != ',' && *p != '*' && i < buffer_len - 1) {
        buffer[i++] = *p++;
    }
    buffer[i] = '\0';
    
    return i > 0;
}

/**
 * @brief Parse GGA sentence (Global Positioning System Fix Data)
 * Format: $GPGGA,hhmmss.ss,llll.ll,a,yyyyy.yy,a,x,xx,x.x,x.x,M,x.x,M,x.x,xxxx*hh
 */
static void parse_gga(const char *sentence)
{
    char field[32];
    
    // Time
    if (get_nmea_field(sentence, 1, field, sizeof(field))) {
        parse_nmea_time(field, &current_gps_data.datetime);
    }
    
    // Latitude
    if (get_nmea_field(sentence, 2, field, sizeof(field)) && strlen(field) > 0) {
        char dir[2];
        if (get_nmea_field(sentence, 3, dir, sizeof(dir))) {
            current_gps_data.latitude = nmea_to_degrees(field, dir[0]);
        }
    }
    
    // Longitude
    if (get_nmea_field(sentence, 4, field, sizeof(field)) && strlen(field) > 0) {
        char dir[2];
        if (get_nmea_field(sentence, 5, dir, sizeof(dir))) {
            current_gps_data.longitude = nmea_to_degrees(field, dir[0]);
        }
    }
    
    // Fix quality
    if (get_nmea_field(sentence, 6, field, sizeof(field))) {
        current_gps_data.fix_quality = (gps_quality_t)atoi(field);
        current_gps_data.valid = (current_gps_data.fix_quality > GPS_QUALITY_INVALID);
    }
    
    // Number of satellites
    if (get_nmea_field(sentence, 7, field, sizeof(field))) {
        current_gps_data.satellites_used = atoi(field);
    }
    
    // HDOP
    if (get_nmea_field(sentence, 8, field, sizeof(field))) {
        current_gps_data.hdop = atof(field);
    }
    
    // Altitude
    if (get_nmea_field(sentence, 9, field, sizeof(field))) {
        current_gps_data.altitude = atof(field);
    }
    
    if (current_gps_data.valid) {
        current_gps_data.last_update_ms = esp_timer_get_time() / 1000;
    }
}

/**
 * @brief Parse RMC sentence (Recommended Minimum)
 * Format: $GPRMC,hhmmss.ss,A,llll.ll,a,yyyyy.yy,a,x.x,x.x,ddmmyy,x.x,a*hh
 */
static void parse_rmc(const char *sentence)
{
    char field[32];
    
    // Time
    if (get_nmea_field(sentence, 1, field, sizeof(field))) {
        parse_nmea_time(field, &current_gps_data.datetime);
    }
    
    // Status (A=valid, V=invalid)
    bool status_valid = false;
    if (get_nmea_field(sentence, 2, field, sizeof(field))) {
        status_valid = (field[0] == 'A');
        current_gps_data.valid = status_valid;
    }
    
    // Latitude
    if (get_nmea_field(sentence, 3, field, sizeof(field)) && strlen(field) > 0) {
        char dir[2];
        if (get_nmea_field(sentence, 4, dir, sizeof(dir))) {
            current_gps_data.latitude = nmea_to_degrees(field, dir[0]);
        }
    }
    
    // Longitude
    if (get_nmea_field(sentence, 5, field, sizeof(field)) && strlen(field) > 0) {
        char dir[2];
        if (get_nmea_field(sentence, 6, dir, sizeof(dir))) {
            current_gps_data.longitude = nmea_to_degrees(field, dir[0]);
        }
    }
    
    // Speed (knots)
    if (get_nmea_field(sentence, 7, field, sizeof(field))) {
        current_gps_data.speed_knots = atof(field);
        current_gps_data.speed_kmh = current_gps_data.speed_knots * 1.852f;
    }
    
    // Course
    if (get_nmea_field(sentence, 8, field, sizeof(field))) {
        current_gps_data.course = atof(field);
    }
    
    // Date
    if (get_nmea_field(sentence, 9, field, sizeof(field))) {
        parse_nmea_date(field, &current_gps_data.datetime);
    }
    
    // Update timestamp
    current_gps_data.last_update_ms = esp_timer_get_time() / 1000;
    
    // Trigger callback if data is valid
    if (status_valid && data_callback) {
        data_callback(&current_gps_data, data_callback_user_data);
    }
}

/**
 * @brief Parse GSA sentence (GPS DOP and active satellites)
 * Format: $GPGSA,A,3,04,05,09,12,,,24,,,,,2.5,1.3,2.1*39
 */
static void parse_gsa(const char *sentence)
{
    char field[32];
    
    // Fix type (1=no fix, 2=2D, 3=3D)
    if (get_nmea_field(sentence, 2, field, sizeof(field))) {
        current_gps_data.fix_type = (gps_fix_type_t)atoi(field);
    }
    
    // Satellite PRNs (fields 3-14) - mark satellites as in use
    for (int i = 3; i <= 14; i++) {
        if (get_nmea_field(sentence, i, field, sizeof(field)) && strlen(field) > 0) {
            int prn = atoi(field);
            // Find and mark satellite as in use
            for (int j = 0; j < satellite_count; j++) {
                if (satellites[j].prn == prn) {
                    satellites[j].in_use = true;
                    break;
                }
            }
        }
    }
    
    // PDOP
    if (get_nmea_field(sentence, 15, field, sizeof(field))) {
        current_gps_data.pdop = atof(field);
    }
    
    // HDOP
    if (get_nmea_field(sentence, 16, field, sizeof(field))) {
        current_gps_data.hdop = atof(field);
    }
    
    // VDOP
    if (get_nmea_field(sentence, 17, field, sizeof(field))) {
        current_gps_data.vdop = atof(field);
    }
}

/**
 * @brief Parse GSV sentence (GPS Satellites in view)
 * Format: $GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00*74
 */
static void parse_gsv(const char *sentence)
{
    char field[32];
    
    // Total number of satellites in view
    if (get_nmea_field(sentence, 3, field, sizeof(field))) {
        current_gps_data.satellites_visible = atoi(field);
    }
    
    // Parse up to 4 satellites per GSV sentence
    for (int i = 0; i < 4; i++) {
        int base_field = 4 + (i * 4);
        
        // PRN
        if (!get_nmea_field(sentence, base_field, field, sizeof(field)) || strlen(field) == 0) {
            break;
        }
        
        if (satellite_count >= GPS_MAX_SATELLITES) break;
        
        int prn = atoi(field);
        
        // Find existing satellite or add new one
        int sat_idx = -1;
        for (int j = 0; j < satellite_count; j++) {
            if (satellites[j].prn == prn) {
                sat_idx = j;
                break;
            }
        }
        
        if (sat_idx == -1) {
            sat_idx = satellite_count++;
            satellites[sat_idx].prn = prn;
            satellites[sat_idx].in_use = false;
        }
        
        // Elevation
        if (get_nmea_field(sentence, base_field + 1, field, sizeof(field))) {
            satellites[sat_idx].elevation = atoi(field);
        }
        
        // Azimuth
        if (get_nmea_field(sentence, base_field + 2, field, sizeof(field))) {
            satellites[sat_idx].azimuth = atoi(field);
        }
        
        // SNR
        if (get_nmea_field(sentence, base_field + 3, field, sizeof(field)) && strlen(field) > 0) {
            satellites[sat_idx].snr = atoi(field);
        }
    }
}

/**
 * @brief Parse NMEA sentence
 */
static void parse_nmea_sentence(const char *sentence)
{
    // Verify checksum
    if (!nmea_verify_checksum(sentence)) {
        ESP_LOGW(TAG, "Invalid checksum: %s", sentence);
        return;
    }
    
    // Call NMEA callback if registered
    if (nmea_callback) {
        nmea_callback(sentence, nmea_callback_user_data);
    }
    
    // Parse based on sentence type
    if (strstr(sentence, "GGA")) {
        parse_gga(sentence);
    } else if (strstr(sentence, "RMC")) {
        // parse_rmc() hands a valid fix to the data callback itself. Calling
        // it again here delivered every fix twice.
        parse_rmc(sentence);
    } else if (strstr(sentence, "GSA")) {
        parse_gsa(sentence);
    } else if (strstr(sentence, "GSV")) {
        parse_gsv(sentence);
    }
}

/**
 * @brief UART RX task - monitors incoming NMEA data
 */
static void gps_rx_task(void *arg)
{
    uart_event_t event;
    uint8_t *buffer = (uint8_t *)malloc(GPS_UART_BUF_SIZE);
    char *nmea_buffer = (char *)malloc(GPS_MAX_NMEA_LEN);
    int nmea_idx = 0;
    
    if (!buffer || !nmea_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RX buffers");
        if (buffer) free(buffer);
        if (nmea_buffer) free(nmea_buffer);
        vTaskDelete(NULL);
        return;
    }
    
    while (1) {
        // Wait for UART events with timeout to allow task yielding
        if (xQueueReceive(uart_queue, (void *)&event, pdMS_TO_TICKS(100))) {
            switch (event.type) {
                case UART_DATA:
                {
                    int len = uart_read_bytes(gps_uart_num, buffer, event.size, pdMS_TO_TICKS(100));
                    
                    for (int i = 0; i < len; i++) {
                        char c = buffer[i];
                        
                        if (c == '$') {
                            // Start of new sentence
                            nmea_idx = 0;
                            nmea_buffer[nmea_idx++] = c;
                        } else if (c == '\n' || c == '\r') {
                            // End of sentence
                            if (nmea_idx > 0) {
                                nmea_buffer[nmea_idx] = '\0';
                                parse_nmea_sentence(nmea_buffer);
                                nmea_idx = 0;
                            }
                        } else if (nmea_idx < GPS_MAX_NMEA_LEN - 1) {
                            nmea_buffer[nmea_idx++] = c;
                        }
                    }
                    break;
                }
                
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG, "UART FIFO overflow");
                    uart_flush_input(gps_uart_num);
                    xQueueReset(uart_queue);
                    nmea_idx = 0;
                    break;
                    
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART ring buffer full");
                    uart_flush_input(gps_uart_num);
                    xQueueReset(uart_queue);
                    nmea_idx = 0;
                    break;
                    
                default:
                    break;
            }
        }
        
        // Yield to other tasks periodically
        taskYIELD();
    }
    
    free(buffer);
    free(nmea_buffer);
    vTaskDelete(NULL);
}

/**
 * @brief Send PAIR command to LC76G module
 */
static esp_err_t gps_send_pair_command(const char *command)
{
    if (!gps_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!command) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Send command
    int len = uart_write_bytes(gps_uart_num, command, strlen(command));
    if (len < 0) {
        ESP_LOGE(TAG, "Failed to send PAIR command: %s", command);
        return ESP_FAIL;
    }
    
    // Send CR+LF
    uart_write_bytes(gps_uart_num, "\r\n", 2);
    
    ESP_LOGI(TAG, "Sent PAIR command: %s", command);
    return ESP_OK;
}

esp_err_t gps_init(void)
{
    return gps_init_with_config(&default_config);
}

esp_err_t gps_init_with_config(const gps_config_t *config)
{
    if (gps_initialized) {
        ESP_LOGW(TAG, "GPS already initialized");
        return ESP_OK;
    }
    
    if (!config) {
        ESP_LOGE(TAG, "Invalid configuration");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Initializing LC76G GNSS module...");
    ESP_LOGI(TAG, "UART: UART%d, TX: GPIO%d, RX: GPIO%d, Baud: %lu", 
             config->uart_num, config->tx_pin, config->rx_pin, config->baud_rate);
    
    // Store configuration
    gps_uart_num = config->uart_num;
    gps_tx_pin = config->tx_pin;
    gps_rx_pin = config->rx_pin;
    
    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    esp_err_t ret;
    
    ret = uart_driver_install(gps_uart_num, GPS_UART_BUF_SIZE * 2, 
                              GPS_UART_BUF_SIZE * 2, 20, &uart_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver");
        return ESP_FAIL;
    }
    
    ret = uart_param_config(gps_uart_num, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART parameters");
        uart_driver_delete(gps_uart_num);
        return ESP_FAIL;
    }
    
    ret = uart_set_pin(gps_uart_num, gps_tx_pin, gps_rx_pin, 
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins");
        uart_driver_delete(gps_uart_num);
        return ESP_FAIL;
    }
    
    // Initialize GPS data structure
    memset(&current_gps_data, 0, sizeof(current_gps_data));
    memset(satellites, 0, sizeof(satellites));
    satellite_count = 0;
    
    gps_initialized = true;
    
    // Create RX task
    ESP_LOGI(TAG, "Starting RX task...");
    xTaskCreate(gps_rx_task, "gps_rx_task", 4096, NULL, 10, &rx_task_handle);
    
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "GPS module initialized successfully!");
    ESP_LOGI(TAG, "Waiting for GPS fix...");
    ESP_LOGI(TAG, "========================================");
    
    return ESP_OK;
}

esp_err_t gps_deinit(void)
{
    if (!gps_initialized) {
        return ESP_OK;
    }
    
    // Delete RX task
    if (rx_task_handle) {
        vTaskDelete(rx_task_handle);
        rx_task_handle = NULL;
    }
    
    // Delete UART driver
    uart_driver_delete(gps_uart_num);
    
    gps_initialized = false;
    data_callback = NULL;
    nmea_callback = NULL;
    
    ESP_LOGI(TAG, "GPS deinitialized");
    return ESP_OK;
}

esp_err_t gps_get_data(gps_data_t *data)
{
    if (!gps_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    
    memcpy(data, &current_gps_data, sizeof(gps_data_t));
    return ESP_OK;
}

esp_err_t gps_get_satellites(gps_satellite_t *sats, uint8_t max_satellites, uint8_t *count)
{
    if (!gps_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!sats || !count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t copy_count = (satellite_count < max_satellites) ? satellite_count : max_satellites;
    memcpy(sats, satellites, copy_count * sizeof(gps_satellite_t));
    *count = copy_count;
    
    return ESP_OK;
}

esp_err_t gps_register_data_callback(gps_data_callback_t callback, void *user_data)
{
    data_callback = callback;
    data_callback_user_data = user_data;
    return ESP_OK;
}

esp_err_t gps_register_nmea_callback(gps_nmea_callback_t callback, void *user_data)
{
    nmea_callback = callback;
    nmea_callback_user_data = user_data;
    return ESP_OK;
}

bool gps_has_fix(void)
{
    return gps_initialized && current_gps_data.valid;
}

gps_fix_type_t gps_get_fix_type(void)
{
    return current_gps_data.fix_type;
}

uint8_t gps_get_satellites_in_use(void)
{
    return current_gps_data.satellites_used;
}

uint32_t gps_get_time_since_update(void)
{
    if (!gps_initialized || current_gps_data.last_update_ms == 0) {
        return UINT32_MAX;
    }
    
    uint64_t now = esp_timer_get_time() / 1000;
    return (uint32_t)(now - current_gps_data.last_update_ms);
}

void gps_format_coordinate(double degrees, bool is_latitude, char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len == 0) return;
    
    char direction;
    if (is_latitude) {
        direction = (degrees >= 0) ? 'N' : 'S';
    } else {
        direction = (degrees >= 0) ? 'E' : 'W';
    }
    
    degrees = fabs(degrees);
    int deg = (int)degrees;
    double minutes = (degrees - deg) * 60.0;
    
    snprintf(buffer, buffer_len, "%d°%.4f'%c", deg, minutes, direction);
}

float gps_calculate_distance(double lat1, double lon1, double lat2, double lon2)
{
    // Haversine formula
    double dLat = (lat2 - lat1) * DEG_TO_RAD;
    double dLon = (lon2 - lon1) * DEG_TO_RAD;
    
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) *
               sin(dLon / 2) * sin(dLon / 2);
    
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    
    return (float)(EARTH_RADIUS_M * c);
}

float gps_calculate_bearing(double lat1, double lon1, double lat2, double lon2)
{
    double dLon = (lon2 - lon1) * DEG_TO_RAD;
    double lat1_rad = lat1 * DEG_TO_RAD;
    double lat2_rad = lat2 * DEG_TO_RAD;
    
    double y = sin(dLon) * cos(lat2_rad);
    double x = cos(lat1_rad) * sin(lat2_rad) -
               sin(lat1_rad) * cos(lat2_rad) * cos(dLon);
    
    double bearing = atan2(y, x) * RAD_TO_DEG;
    
    // Normalize to 0-360
    bearing = fmod(bearing + 360.0, 360.0);
    
    return (float)bearing;
}

/* ========================================
 * LC76G-Specific PAIR Command Functions
 * ======================================== */

esp_err_t gps_set_update_rate(uint8_t rate_hz)
{
    if (!gps_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (rate_hz < 1 || rate_hz > GPS_UPDATE_RATE_MAX) {
        ESP_LOGE(TAG, "Invalid update rate: %d Hz (valid: 1-%d)", rate_hz, GPS_UPDATE_RATE_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Convert Hz to interval in ms
    uint16_t interval_ms = 1000 / rate_hz;
    
    // Format: $PAIR050,<interval>*<checksum>
    char command[32];
    snprintf(command, sizeof(command), "$PAIR050,%d", interval_ms);
    
    // Calculate checksum
    uint8_t checksum = nmea_checksum(command);
    
    // Append checksum
    char full_command[64];
    snprintf(full_command, sizeof(full_command), "%s*%02X", command, checksum);
    
    ESP_LOGI(TAG, "Setting update rate to %d Hz (%d ms interval)", rate_hz, interval_ms);
    return gps_send_pair_command(full_command);
}

esp_err_t gps_configure_constellations(bool gps_enable, bool glonass_enable, 
                                       bool galileo_enable, bool beidou_enable, 
                                       bool qzss_enable)
{
    if (!gps_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Format: $PAIR066,<GPS>,<GLONASS>,<Galileo>,<BDS>,<QZSS>,0*<checksum>
    char command[64];
    snprintf(command, sizeof(command), "$PAIR066,%d,%d,%d,%d,%d,0",
             gps_enable ? 1 : 0,
             glonass_enable ? 1 : 0,
             galileo_enable ? 1 : 0,
             beidou_enable ? 1 : 0,
             qzss_enable ? 1 : 0);
    
    // Calculate checksum
    uint8_t checksum = nmea_checksum(command);
    
    // Append checksum
    char full_command[128];
    snprintf(full_command, sizeof(full_command), "%s*%02X", command, checksum);
    
    ESP_LOGI(TAG, "Configuring constellations: GPS=%d GLONASS=%d Galileo=%d BeiDou=%d QZSS=%d",
             gps_enable, glonass_enable, galileo_enable, beidou_enable, qzss_enable);
    
    return gps_send_pair_command(full_command);
}

esp_err_t gps_hot_start(void)
{
    ESP_LOGI(TAG, "Performing hot start (using all available data)");
    return gps_send_pair_command("$PAIR004*3E");
}

esp_err_t gps_warm_start(void)
{
    ESP_LOGI(TAG, "Performing warm start (clearing ephemeris)");
    return gps_send_pair_command("$PAIR005*3F");
}

esp_err_t gps_cold_start(void)
{
    ESP_LOGI(TAG, "Performing cold start (clearing all data)");
    return gps_send_pair_command("$PAIR006*3C");
}

esp_err_t gps_factory_reset(void)
{
    ESP_LOGI(TAG, "Performing factory reset");
    return gps_send_pair_command("$PAIR007*3D");
}

esp_err_t gps_set_baud_rate(uint32_t baudrate)
{
    // Validate common baud rates
    if (baudrate != 4800 && baudrate != 9600 && baudrate != 19200 && 
        baudrate != 38400 && baudrate != 57600 && baudrate != 115200 &&
        baudrate != 230400 && baudrate != 460800 && baudrate != 921600) {
        ESP_LOGE(TAG, "Invalid baud rate: %lu", baudrate);
        return ESP_ERR_INVALID_ARG;
    }
    
    char command[64];
    snprintf(command, sizeof(command), "$PAIR864,0,0,%lu", baudrate);
    
    // Calculate checksum
    uint8_t checksum = nmea_checksum(command);
    
    // Append checksum
    char full_command[80];
    snprintf(full_command, sizeof(full_command), "%s*%02X", command, checksum);
    
    ESP_LOGI(TAG, "Setting baud rate to %lu", baudrate);
    esp_err_t ret = gps_send_pair_command(full_command);
    
    if (ret == ESP_OK) {
        ESP_LOGW(TAG, "Baud rate changed to %lu. UART must be reconfigured!", baudrate);
        ESP_LOGW(TAG, "Call gps_deinit() then gps_init_with_config() with new baud rate");
    }
    
    return ret;
}
