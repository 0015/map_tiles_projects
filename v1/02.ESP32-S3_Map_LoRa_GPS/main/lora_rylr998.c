/* RYLR998 LoRa module implementation */

#include "lora_rylr998.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "LoRa_RYLR998";

static bool lora_initialized = false;
static lora_rx_callback_t rx_callback = NULL;
static void *rx_callback_user_data = NULL;
static QueueHandle_t uart_queue = NULL;
static TaskHandle_t rx_task_handle = NULL;
static SemaphoreHandle_t uart_mutex = NULL;

static const lora_config_t default_config = {
    .frequency = LORA_DEFAULT_BAND,
    .spreading_factor = LORA_SF9,       
    .bandwidth = LORA_BW_125KHZ,
    .coding_rate = LORA_CR_4_5,
    .preamble = LORA_PREAMBLE_12,
    .address = 1,
    .tx_power = 22,                     
    .password = "EEDCAA90",
};

static bool parse_rcv_message(const char *response, lora_rx_msg_t *msg)
{
    ESP_LOGI(TAG, "Parsing RCV message: %s", response);
    
    if (strncmp(response, "+RCV=", 5) != 0) {
        ESP_LOGW(TAG, "Not a +RCV message");
        return false;
    }

    const char *p = response + 5;

    msg->sender_address = atoi(p);
    p = strchr(p, ',');
    if (!p) {
        ESP_LOGE(TAG, "Parse error: no comma after address");
        return false;
    }
    p++;

    msg->payload_length = atoi(p);
    p = strchr(p, ',');
    if (!p) {
        ESP_LOGE(TAG, "Parse error: no comma after length");
        return false;
    }
    p++;

    while (*p == ' ') p++;

    if (msg->payload_length > LORA_MAX_PAYLOAD_LEN) {
        ESP_LOGE(TAG, "Payload length too large: %u", msg->payload_length);
        return false;
    }
    
    memcpy(msg->payload, p, msg->payload_length);
    msg->payload[msg->payload_length] = '\0';  

    p += msg->payload_length;

    if (*p == ',') {
        p++;
        while (*p == ' ') p++;  
        msg->rssi = atoi(p);

        p = strchr(p, ',');
        if (p) {
            p++;
            while (*p == ' ') p++;  
            msg->snr = atoi(p);
        }
    }
    
    ESP_LOGI(TAG, "Parsed: addr=%u, len=%u, data=%s, rssi=%d, snr=%d",
             msg->sender_address, msg->payload_length, msg->payload, msg->rssi, msg->snr);
    
    return true;
}

bool lora_parse_gps_data(const lora_rx_msg_t *msg, lora_gps_data_t *gps_data)
{
    if (!msg || !gps_data) {
        return false;
    }

    gps_data->device_id = msg->sender_address;
    gps_data->rssi = msg->rssi;
    gps_data->snr = msg->snr;
    gps_data->valid = false;
    gps_data->device_type = 0;
    strcpy(gps_data->device_name, "Unknown");

    char payload_str[LORA_MAX_PAYLOAD_LEN + 1];
    memcpy(payload_str, msg->payload, msg->payload_length);
    payload_str[msg->payload_length] = '\0';

    char name[16];
    int type;
    int parsed = sscanf(payload_str, "%15[^,],%d,%lf,%lf", 
                       name, &type, &gps_data->latitude, &gps_data->longitude);
    
    if (parsed == 4) {
        
        strncpy(gps_data->device_name, name, sizeof(gps_data->device_name) - 1);
        gps_data->device_name[sizeof(gps_data->device_name) - 1] = '\0';
        gps_data->device_type = (uint8_t)type;
        ESP_LOGI("LORA_PARSE", "Parsed GPS: '%s' -> Name=%s, Type=%d, Lat=%.8f, Lon=%.8f", 
                 payload_str, name, type, gps_data->latitude, gps_data->longitude);
    } else {
        
        parsed = sscanf(payload_str, "%lf,%lf", &gps_data->latitude, &gps_data->longitude);
        if (parsed != 2) {
            return false;
        }
        
        strcpy(gps_data->device_name, "Device");
        gps_data->device_type = 0;
    }

    if (gps_data->latitude >= -90.0 && gps_data->latitude <= 90.0 &&
        gps_data->longitude >= -180.0 && gps_data->longitude <= 180.0) {
        gps_data->valid = true;
        return true;
    }
    
    return false;
}

static void lora_rx_task(void *arg)
{
    uart_event_t event;
    uint8_t *dtmp = (uint8_t *)malloc(LORA_UART_BUF_SIZE);
    
    if (!dtmp) {
        ESP_LOGE(TAG, "Failed to allocate RX buffer");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "RX task started and running - waiting for messages...");
    
    while (1) {
        if (xQueueReceive(uart_queue, (void *)&event, pdMS_TO_TICKS(5000))) {            
            switch (event.type) {
                case UART_DATA:
                {
                    
                    if (uart_mutex && xSemaphoreTake(uart_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                        int len = uart_read_bytes(LORA_UART_NUM, dtmp, event.size, pdMS_TO_TICKS(100));
                        if (len > 0) {
                            dtmp[len] = '\0';
                            
                            ESP_LOGI(TAG, "UART RX (%d bytes): %s", len, dtmp);

                            if (strncmp((char *)dtmp, "+RCV=", 5) == 0) {
                                ESP_LOGI(TAG, "Received: %s", dtmp);
                                if (rx_callback) {
                                    lora_rx_msg_t msg = {0};
                                    if (parse_rcv_message((char *)dtmp, &msg)) {
                                        rx_callback(&msg, rx_callback_user_data);
                                    }
                                } else {
                                    ESP_LOGW(TAG, "No RX callback registered!");
                                }
                            }
                        }
                        xSemaphoreGive(uart_mutex);
                        vTaskDelay(pdMS_TO_TICKS(5)); 
                    }
                    break;
                }
                
                case UART_FIFO_OVF:
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG, "UART overflow");
                    uart_flush_input(LORA_UART_NUM);
                    xQueueReset(uart_queue);
                    break;
                    
                default:
                    break;
            }
        }
    }
    
    free(dtmp);
    vTaskDelete(NULL);
}

esp_err_t lora_send_at_command(const char *cmd, char *response, size_t response_len, uint32_t timeout_ms)
{
    if (!lora_initialized) {
        ESP_LOGE(TAG, "LoRa not initialized");
        return ESP_FAIL;
    }

    if (uart_mutex && xSemaphoreTake(uart_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire UART mutex");
        return ESP_FAIL;
    }

    uart_flush(LORA_UART_NUM);
    
    char cmd_buf[512];
    snprintf(cmd_buf, sizeof(cmd_buf), "%s\r\n", cmd);
    int written = uart_write_bytes(LORA_UART_NUM, cmd_buf, strlen(cmd_buf));
    
    if (written < 0) {
        ESP_LOGE(TAG, "Failed to write: %s", cmd);
        if (uart_mutex) xSemaphoreGive(uart_mutex);
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    if (response && response_len > 0) {
        memset(response, 0, response_len);
        int len = uart_read_bytes(LORA_UART_NUM, (uint8_t *)response, response_len - 1, 
                                  pdMS_TO_TICKS(timeout_ms));
        
        if (len > 0) {
            response[len] = '\0';
            
            char *p = strstr(response, "\r\n");
            if (p) *p = '\0';
            ret = ESP_OK;
        } else {
            ret = ESP_ERR_TIMEOUT;
        }
    }

    if (uart_mutex) xSemaphoreGive(uart_mutex);
    
    return ret;
}

esp_err_t lora_init(void)
{
    return lora_init_with_config(&default_config);
}

esp_err_t lora_init_with_config(const lora_config_t *config)
{
    if (lora_initialized) {
        ESP_LOGW(TAG, "LoRa already initialized");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Initializing RYLR998 LoRa module...");
    ESP_LOGI(TAG, "UART: UART%d, TX: GPIO%d, RX: GPIO%d, Baud: %d", 
             LORA_UART_NUM, LORA_UART_TX_PIN, LORA_UART_RX_PIN, LORA_UART_BAUD_RATE);

    uart_config_t uart_config = {
        .baud_rate = LORA_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    esp_err_t ret;
    
    ret = uart_driver_install(LORA_UART_NUM, LORA_UART_BUF_SIZE * 2, 
                              LORA_UART_BUF_SIZE * 2, 20, &uart_queue, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver");
        return ESP_FAIL;
    }
    
    ret = uart_param_config(LORA_UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART parameters");
        uart_driver_delete(LORA_UART_NUM);
        return ESP_FAIL;
    }
    
    ret = uart_set_pin(LORA_UART_NUM, LORA_UART_TX_PIN, LORA_UART_RX_PIN, 
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins");
        uart_driver_delete(LORA_UART_NUM);
        return ESP_FAIL;
    }

    uart_mutex = xSemaphoreCreateMutex();
    if (!uart_mutex) {
        ESP_LOGE(TAG, "Failed to create UART mutex");
        uart_driver_delete(LORA_UART_NUM);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "UART mutex created");
    
    lora_initialized = true;

    vTaskDelay(pdMS_TO_TICKS(1000));

    if (config) {

        ESP_LOGI(TAG, "Setting frequency to %lu Hz...", config->frequency);
        if (lora_set_band(config->frequency) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set frequency");
        }

        ESP_LOGI(TAG, "Setting parameters: SF=%d, BW=%d, CR=%d, Preamble=%d...", 
                 config->spreading_factor, config->bandwidth, config->coding_rate, config->preamble);
        if (lora_set_parameter(config->spreading_factor, config->bandwidth, 
                               config->coding_rate, config->preamble) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set parameters");
        }

        ESP_LOGI(TAG, "Setting address to %u...", config->address);
        if (lora_set_address(config->address) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set address");
        }

        ESP_LOGI(TAG, "Setting network ID to 3...");
        if (lora_set_network_id(3) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set network ID");
        }

        if (config->password[0] != '\0') {
            ESP_LOGI(TAG, "Setting password to %s...", config->password);
            if (lora_set_password(config->password) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to set password");
            }
        }

        ESP_LOGI(TAG, "Setting TX power to %d dBm...", config->tx_power);
        if (lora_set_tx_power(config->tx_power) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set TX power");
        }
        
        ESP_LOGI(TAG, "Configuration completed");
    }

    xTaskCreate(lora_rx_task, "lora_rx", 4096, NULL, 10, &rx_task_handle);
    
    ESP_LOGI(TAG, "LoRa module initialized successfully");
    return ESP_OK;
}

esp_err_t lora_deinit(void)
{
    if (!lora_initialized) {
        return ESP_OK;
    }

    if (rx_task_handle) {
        vTaskDelete(rx_task_handle);
        rx_task_handle = NULL;
    }

    uart_driver_delete(LORA_UART_NUM);
    
    lora_initialized = false;
    rx_callback = NULL;
    rx_callback_user_data = NULL;
    
    ESP_LOGI(TAG, "LoRa module deinitialized");
    return ESP_OK;
}

esp_err_t lora_test(void)
{
    char response[64];
    esp_err_t ret = lora_send_at_command("AT", response, sizeof(response), 1000);
    
    if (ret == ESP_OK && strstr(response, "+OK") != NULL) {
        ESP_LOGI(TAG, "LoRa module test: OK");
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "LoRa module test: FAILED");
    return ESP_FAIL;
}

esp_err_t lora_reset(void)
{
    char response[64];
    esp_err_t ret = lora_send_at_command("AT+RESET", response, sizeof(response), 3000);
    
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2000)); 
        return ESP_OK;
    }
    
    return ESP_FAIL;
}

esp_err_t lora_set_band(uint32_t frequency)
{
    char cmd[64];
    char response[64];
    
    snprintf(cmd, sizeof(cmd), "AT+BAND=%lu", frequency);
    esp_err_t ret = lora_send_at_command(cmd, response, sizeof(response), 2000);
    
    if (ret == ESP_OK && strstr(response, "+OK") != NULL) {
        ESP_LOGI(TAG, "Frequency set to %lu Hz", frequency);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to set frequency");
    return ESP_FAIL;
}

esp_err_t lora_set_parameter(lora_spreading_factor_t sf, lora_bandwidth_t bw, 
                              lora_coding_rate_t cr, lora_preamble_t preamble)
{
    char cmd[64];
    char response[64];
    
    snprintf(cmd, sizeof(cmd), "AT+PARAMETER=%d,%d,%d,%d", sf, bw, cr, preamble);
    esp_err_t ret = lora_send_at_command(cmd, response, sizeof(response), 2000);
    
    if (ret == ESP_OK && strstr(response, "+OK") != NULL) {
        ESP_LOGI(TAG, "Parameters set: SF=%d, BW=%d, CR=%d, Preamble=%d", sf, bw, cr, preamble);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to set parameters");
    return ESP_FAIL;
}

esp_err_t lora_set_address(uint16_t address)
{
    char cmd[64];
    char response[64];
    
    snprintf(cmd, sizeof(cmd), "AT+ADDRESS=%u", address);
    esp_err_t ret = lora_send_at_command(cmd, response, sizeof(response), 2000);
    
    if (ret == ESP_OK && strstr(response, "+OK") != NULL) {
        ESP_LOGI(TAG, "Address set to %u", address);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to set address");
    return ESP_FAIL;
}

esp_err_t lora_set_network_id(uint8_t network_id)
{
    char cmd[64];
    char response[64];
    
    snprintf(cmd, sizeof(cmd), "AT+NETWORKID=%u", network_id);
    esp_err_t ret = lora_send_at_command(cmd, response, sizeof(response), 2000);
    
    if (ret == ESP_OK && strstr(response, "+OK") != NULL) {
        ESP_LOGI(TAG, "Network ID set to %u", network_id);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to set network ID");
    return ESP_FAIL;
}

esp_err_t lora_set_password(const char *password)
{
    if (!password || strlen(password) != 8) {
        ESP_LOGE(TAG, "Invalid password format - must be 8 hex characters");
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < 8; i++) {
        if (!isxdigit((unsigned char)password[i])) {
            ESP_LOGE(TAG, "Invalid password - must contain only hex characters (0-9, A-F)");
            return ESP_ERR_INVALID_ARG;
        }
    }
    
    char cmd[64];
    char response[64];
    
    snprintf(cmd, sizeof(cmd), "AT+CPIN=%s", password);
    esp_err_t ret = lora_send_at_command(cmd, response, sizeof(response), 2000);
    
    if (ret == ESP_OK && strstr(response, "+OK") != NULL) {
        ESP_LOGI(TAG, "Password set to %s", password);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to set password");
    return ESP_FAIL;
}

esp_err_t lora_set_tx_power(int8_t power)
{
    if (power < 0 || power > 22) {
        ESP_LOGE(TAG, "Invalid TX power: %d (valid range: 0-22)", power);
        return ESP_ERR_INVALID_ARG;
    }
    
    char cmd[64];
    char response[64];
    
    snprintf(cmd, sizeof(cmd), "AT+CRFOP=%d", power);
    esp_err_t ret = lora_send_at_command(cmd, response, sizeof(response), 2000);
    
    if (ret == ESP_OK && strstr(response, "+OK") != NULL) {
        ESP_LOGI(TAG, "TX power set to %d dBm", power);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to set TX power");
    return ESP_FAIL;
}

esp_err_t lora_send(uint16_t dest_addr, const uint8_t *data, uint8_t len)
{
    if (!data || len == 0 || len > LORA_MAX_PAYLOAD_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    
    char cmd[512];
    char response[64];

    snprintf(cmd, sizeof(cmd), "AT+SEND=%u,%u,%.*s", dest_addr, len, len, (char *)data);
    
    ESP_LOGI(TAG, "TX -> Addr %u: %.*s", dest_addr, len, (char *)data);
    ESP_LOGI(TAG, "AT Command: %s (len=%d)", cmd, strlen(cmd));
    
    esp_err_t ret = lora_send_at_command(cmd, response, sizeof(response), 1000);
    
    if (ret == ESP_OK && strstr(response, "+OK") != NULL) {
        ESP_LOGI(TAG, "TX confirmed: %s", response);
        return ESP_OK;
    }
    
    ESP_LOGW(TAG, "TX failed: %s (ret=%d)", response, ret);
    return ESP_FAIL;
}

esp_err_t lora_send_string(uint16_t dest_addr, const char *str)
{
    if (!str) {
        return ESP_ERR_INVALID_ARG;
    }
    
    size_t len = strlen(str);
    if (len > LORA_MAX_PAYLOAD_LEN) {
        ESP_LOGE(TAG, "String too long (%zu bytes, max %d)", len, LORA_MAX_PAYLOAD_LEN);
        return ESP_ERR_INVALID_SIZE;
    }
    
    return lora_send(dest_addr, (const uint8_t *)str, len);
}

esp_err_t lora_send_string_with_ack(uint16_t dest_addr, const char *str, uint32_t ack_timeout_ms)
{
    if (!str) {
        return ESP_ERR_INVALID_ARG;
    }
    
    size_t len = strlen(str);
    if (len > LORA_MAX_PAYLOAD_LEN) {
        ESP_LOGE(TAG, "String too long (%zu bytes, max %d)", len, LORA_MAX_PAYLOAD_LEN);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = lora_send(dest_addr, (const uint8_t *)str, len);
    if (ret != ESP_OK) {
        return ret;
    }

    char response[128];
    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(ack_timeout_ms);
    
    while ((xTaskGetTickCount() - start_time) < timeout_ticks) {
        
        int len = uart_read_bytes(LORA_UART_NUM, (uint8_t *)response, sizeof(response) - 1, 
                                  pdMS_TO_TICKS(100));
        
        if (len > 0) {
            response[len] = '\0';

            if (strstr(response, "+RCV=") && strstr(response, "ACK")) {
                
                char *p = strstr(response, "+RCV=");
                if (p) {
                    p += 5;
                    uint16_t sender = atoi(p);
                    if (sender == dest_addr) {
                        ESP_LOGI(TAG, "ACK received from address %u", dest_addr);
                        return ESP_OK;
                    }
                }
            }
        }
    }
    
    ESP_LOGW(TAG, "ACK timeout - no response from address %u within %lu ms", dest_addr, ack_timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t lora_register_rx_callback(lora_rx_callback_t callback, void *user_data)
{
    rx_callback = callback;
    rx_callback_user_data = user_data;
    ESP_LOGI(TAG, "RX callback registered: %p", (void*)callback);
    return ESP_OK;
}

esp_err_t lora_enter_power_save_mode(uint16_t rx_time, uint16_t low_speed_time)
{
    if (rx_time < 100 || rx_time > 60000 || low_speed_time < 100 || low_speed_time > 60000) {
        ESP_LOGE(TAG, "Invalid power save parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    char cmd[64];
    char response[64];
    
    snprintf(cmd, sizeof(cmd), "AT+MODE=2,%u,%u", rx_time, low_speed_time);
    esp_err_t ret = lora_send_at_command(cmd, response, sizeof(response), 2000);
    
    if (ret == ESP_OK && strstr(response, "+OK") != NULL) {
        ESP_LOGI(TAG, "Entered power save mode: RX=%u ms, Low Speed=%u ms", rx_time, low_speed_time);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to enter power save mode");
    return ESP_FAIL;
}

esp_err_t lora_get_version(char *version, size_t len)
{
    if (!version || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char response[128];
    esp_err_t ret = lora_send_at_command("AT+VER?", response, sizeof(response), 2000);
    
    if (ret == ESP_OK) {
        strncpy(version, response, len - 1);
        version[len - 1] = '\0';
        return ESP_OK;
    }
    
    return ESP_FAIL;
}

esp_err_t lora_get_signal_quality(int16_t *rssi, int8_t *snr)
{
    if (!rssi || !snr) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char response[128];
    esp_err_t ret = lora_send_at_command("AT+SNR?", response, sizeof(response), 2000);
    
    if (ret == ESP_OK) {
        
        char *p = strstr(response, "+SNR=");
        if (p) {
            p += 5;
            *snr = atoi(p);
            p = strchr(p, ',');
            if (p) {
                p++;
                *rssi = atoi(p);
                return ESP_OK;
            }
        }
    }
    
    return ESP_FAIL;
}
