/* LC76G GPS module implementation */

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

static uint8_t gps_uart_num = GPS_UART_NUM;
static int gps_tx_pin = GPS_UART_TX_PIN;
static int gps_rx_pin = GPS_UART_RX_PIN;

static const gps_config_t default_config = {
    .uart_num = GPS_UART_NUM,
    .tx_pin = GPS_UART_TX_PIN,
    .rx_pin = GPS_UART_RX_PIN,
    .baud_rate = GPS_UART_BAUD_RATE,
    .update_rate_hz = GPS_UPDATE_RATE_DEFAULT,
};

#define EARTH_RADIUS_M 6371000.0
#define DEG_TO_RAD (M_PI / 180.0)
#define RAD_TO_DEG (180.0 / M_PI)

static uint8_t nmea_checksum(const char *sentence)
{
    uint8_t checksum = 0;
    const char *p = sentence;
    
    if (*p == '$') p++;
    
    while (*p && *p != '*') {
        checksum ^= *p;
        p++;
    }
    
    return checksum;
}

static bool nmea_verify_checksum(const char *sentence)
{
    const char *p = strchr(sentence, '*');
    if (!p) return false;
    
    uint8_t expected = nmea_checksum(sentence);
    uint8_t actual = (uint8_t)strtol(p + 1, NULL, 16);
    
    return expected == actual;
}

static double nmea_to_degrees(const char *coord, char direction)
{
    if (!coord || strlen(coord) < 4) return 0.0;
    
    char *dot = strchr(coord, '.');
    if (!dot) return 0.0;
    
    int deg_len = (direction == 'N' || direction == 'S') ? 2 : 3;
    
    char deg_str[4] = {0};
    strncpy(deg_str, coord, deg_len);
    double degrees = atof(deg_str);
    
    double minutes = atof(coord + deg_len);
    
    double result = degrees + (minutes / 60.0);
    
    if (direction == 'S' || direction == 'W') {
        result = -result;
    }
    
    return result;
}

static void parse_nmea_time(const char *time_str, gps_datetime_t *datetime)
{
    if (!time_str || strlen(time_str) < 6) return;
    
    char buf[3] = {0};
    
    buf[0] = time_str[0];
    buf[1] = time_str[1];
    buf[2] = '\0';
    datetime->hour = atoi(buf);
    
    buf[0] = time_str[2];
    buf[1] = time_str[3];
    buf[2] = '\0';
    datetime->minute = atoi(buf);
    
    buf[0] = time_str[4];
    buf[1] = time_str[5];
    buf[2] = '\0';
    datetime->second = atoi(buf);
    
    const char *dot = strchr(time_str, '.');
    if (dot) {
        datetime->millisecond = (uint16_t)(atof(dot) * 1000);
    }
}

static void parse_nmea_date(const char *date_str, gps_datetime_t *datetime)
{
    if (!date_str || strlen(date_str) < 6) return;
    
    char buf[3] = {0};

    buf[0] = date_str[0];
    buf[1] = date_str[1];
    buf[2] = '\0';
    datetime->day = atoi(buf);

    buf[0] = date_str[2];
    buf[1] = date_str[3];
    buf[2] = '\0';
    datetime->month = atoi(buf);

    buf[0] = date_str[4];
    buf[1] = date_str[5];
    buf[2] = '\0';
    datetime->year = 2000 + atoi(buf);
}

static bool get_nmea_field(const char *sentence, int field_num, char *buffer, size_t buffer_len)
{
    if (!sentence || !buffer || buffer_len == 0) return false;
    
    const char *p = sentence;
    int current_field = 0;

    while (*p && current_field < field_num) {
        if (*p == ',') current_field++;
        p++;
    }
    
    if (current_field != field_num) return false;

    size_t i = 0;
    while (*p && *p != ',' && *p != '*' && i < buffer_len - 1) {
        buffer[i++] = *p++;
    }
    buffer[i] = '\0';
    
    return i > 0;
}

static void parse_gga(const char *sentence)
{
    char field[32];

    if (get_nmea_field(sentence, 1, field, sizeof(field))) {
        parse_nmea_time(field, &current_gps_data.datetime);
    }

    if (get_nmea_field(sentence, 2, field, sizeof(field)) && strlen(field) > 0) {
        char dir[2];
        if (get_nmea_field(sentence, 3, dir, sizeof(dir))) {
            current_gps_data.latitude = nmea_to_degrees(field, dir[0]);
        }
    }

    if (get_nmea_field(sentence, 4, field, sizeof(field)) && strlen(field) > 0) {
        char dir[2];
        if (get_nmea_field(sentence, 5, dir, sizeof(dir))) {
            current_gps_data.longitude = nmea_to_degrees(field, dir[0]);
        }
    }

    if (get_nmea_field(sentence, 6, field, sizeof(field))) {
        current_gps_data.fix_quality = (gps_quality_t)atoi(field);
        current_gps_data.valid = (current_gps_data.fix_quality > GPS_QUALITY_INVALID);
    }

    if (get_nmea_field(sentence, 7, field, sizeof(field))) {
        current_gps_data.satellites_used = atoi(field);
    }

    if (get_nmea_field(sentence, 8, field, sizeof(field))) {
        current_gps_data.hdop = atof(field);
    }

    if (get_nmea_field(sentence, 9, field, sizeof(field))) {
        current_gps_data.altitude = atof(field);
    }
    
    if (current_gps_data.valid) {
        current_gps_data.last_update_ms = esp_timer_get_time() / 1000;
    }
}

static void parse_rmc(const char *sentence)
{
    char field[32];

    if (get_nmea_field(sentence, 1, field, sizeof(field))) {
        parse_nmea_time(field, &current_gps_data.datetime);
    }

    bool status_valid = false;
    if (get_nmea_field(sentence, 2, field, sizeof(field))) {
        status_valid = (field[0] == 'A');
        current_gps_data.valid = status_valid;
    }

    if (get_nmea_field(sentence, 3, field, sizeof(field)) && strlen(field) > 0) {
        char dir[2];
        if (get_nmea_field(sentence, 4, dir, sizeof(dir))) {
            current_gps_data.latitude = nmea_to_degrees(field, dir[0]);
        }
    }

    if (get_nmea_field(sentence, 5, field, sizeof(field)) && strlen(field) > 0) {
        char dir[2];
        if (get_nmea_field(sentence, 6, dir, sizeof(dir))) {
            current_gps_data.longitude = nmea_to_degrees(field, dir[0]);
        }
    }

    if (get_nmea_field(sentence, 7, field, sizeof(field))) {
        current_gps_data.speed_knots = atof(field);
        current_gps_data.speed_kmh = current_gps_data.speed_knots * 1.852f;
    }

    if (get_nmea_field(sentence, 8, field, sizeof(field))) {
        current_gps_data.course = atof(field);
    }

    if (get_nmea_field(sentence, 9, field, sizeof(field))) {
        parse_nmea_date(field, &current_gps_data.datetime);
    }

    current_gps_data.last_update_ms = esp_timer_get_time() / 1000;

    if (status_valid && data_callback) {
        data_callback(&current_gps_data, data_callback_user_data);
    }
}

static void parse_gsa(const char *sentence)
{
    char field[32];

    if (get_nmea_field(sentence, 2, field, sizeof(field))) {
        current_gps_data.fix_type = (gps_fix_type_t)atoi(field);
    }

    for (int i = 3; i <= 14; i++) {
        if (get_nmea_field(sentence, i, field, sizeof(field)) && strlen(field) > 0) {
            int prn = atoi(field);
            
            for (int j = 0; j < satellite_count; j++) {
                if (satellites[j].prn == prn) {
                    satellites[j].in_use = true;
                    break;
                }
            }
        }
    }

    if (get_nmea_field(sentence, 15, field, sizeof(field))) {
        current_gps_data.pdop = atof(field);
    }

    if (get_nmea_field(sentence, 16, field, sizeof(field))) {
        current_gps_data.hdop = atof(field);
    }

    if (get_nmea_field(sentence, 17, field, sizeof(field))) {
        current_gps_data.vdop = atof(field);
    }
}

static void parse_gsv(const char *sentence)
{
    char field[32];

    if (get_nmea_field(sentence, 3, field, sizeof(field))) {
        current_gps_data.satellites_visible = atoi(field);
    }

    for (int i = 0; i < 4; i++) {
        int base_field = 4 + (i * 4);

        if (!get_nmea_field(sentence, base_field, field, sizeof(field)) || strlen(field) == 0) {
            break;
        }
        
        if (satellite_count >= GPS_MAX_SATELLITES) break;
        
        int prn = atoi(field);

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

        if (get_nmea_field(sentence, base_field + 1, field, sizeof(field))) {
            satellites[sat_idx].elevation = atoi(field);
        }

        if (get_nmea_field(sentence, base_field + 2, field, sizeof(field))) {
            satellites[sat_idx].azimuth = atoi(field);
        }

        if (get_nmea_field(sentence, base_field + 3, field, sizeof(field)) && strlen(field) > 0) {
            satellites[sat_idx].snr = atoi(field);
        }
    }
}

static void parse_nmea_sentence(const char *sentence)
{
    
    if (!nmea_verify_checksum(sentence)) {
        ESP_LOGW(TAG, "Invalid checksum: %s", sentence);
        return;
    }

    if (nmea_callback) {
        nmea_callback(sentence, nmea_callback_user_data);
    }

    if (strstr(sentence, "GGA")) {
        parse_gga(sentence);
    } else if (strstr(sentence, "RMC")) {
        parse_rmc(sentence);
        
        if (data_callback && current_gps_data.valid) {
            data_callback(&current_gps_data, data_callback_user_data);
        }
    } else if (strstr(sentence, "GSA")) {
        parse_gsa(sentence);
    } else if (strstr(sentence, "GSV")) {
        parse_gsv(sentence);
    }
}

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
        
        if (xQueueReceive(uart_queue, (void *)&event, pdMS_TO_TICKS(100))) {
            switch (event.type) {
                case UART_DATA:
                {
                    int len = uart_read_bytes(gps_uart_num, buffer, event.size, pdMS_TO_TICKS(100));
                    
                    for (int i = 0; i < len; i++) {
                        char c = buffer[i];
                        
                        if (c == '$') {
                            
                            nmea_idx = 0;
                            nmea_buffer[nmea_idx++] = c;
                        } else if (c == '\n' || c == '\r') {
                            
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

        taskYIELD();
    }
    
    free(buffer);
    free(nmea_buffer);
    vTaskDelete(NULL);
}

static esp_err_t gps_send_pair_command(const char *command)
{
    if (!gps_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!command) {
        return ESP_ERR_INVALID_ARG;
    }

    int len = uart_write_bytes(gps_uart_num, command, strlen(command));
    if (len < 0) {
        ESP_LOGE(TAG, "Failed to send PAIR command: %s", command);
        return ESP_FAIL;
    }

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

    gps_uart_num = config->uart_num;
    gps_tx_pin = config->tx_pin;
    gps_rx_pin = config->rx_pin;

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

    memset(&current_gps_data, 0, sizeof(current_gps_data));
    memset(satellites, 0, sizeof(satellites));
    satellite_count = 0;
    
    gps_initialized = true;

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

    if (rx_task_handle) {
        vTaskDelete(rx_task_handle);
        rx_task_handle = NULL;
    }

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

    bearing = fmod(bearing + 360.0, 360.0);
    
    return (float)bearing;
}

esp_err_t gps_set_update_rate(uint8_t rate_hz)
{
    if (!gps_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    if (rate_hz < 1 || rate_hz > GPS_UPDATE_RATE_MAX) {
        ESP_LOGE(TAG, "Invalid update rate: %d Hz (valid: 1-%d)", rate_hz, GPS_UPDATE_RATE_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t interval_ms = 1000 / rate_hz;

    char command[32];
    snprintf(command, sizeof(command), "$PAIR050,%d", interval_ms);

    uint8_t checksum = nmea_checksum(command);

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

    char command[64];
    snprintf(command, sizeof(command), "$PAIR066,%d,%d,%d,%d,%d,0",
             gps_enable ? 1 : 0,
             glonass_enable ? 1 : 0,
             galileo_enable ? 1 : 0,
             beidou_enable ? 1 : 0,
             qzss_enable ? 1 : 0);

    uint8_t checksum = nmea_checksum(command);

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
    
    if (baudrate != 4800 && baudrate != 9600 && baudrate != 19200 && 
        baudrate != 38400 && baudrate != 57600 && baudrate != 115200 &&
        baudrate != 230400 && baudrate != 460800 && baudrate != 921600) {
        ESP_LOGE(TAG, "Invalid baud rate: %lu", baudrate);
        return ESP_ERR_INVALID_ARG;
    }
    
    char command[64];
    snprintf(command, sizeof(command), "$PAIR864,0,0,%lu", baudrate);

    uint8_t checksum = nmea_checksum(command);

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
