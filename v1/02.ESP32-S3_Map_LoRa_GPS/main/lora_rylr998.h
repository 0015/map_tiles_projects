/* RYLR998 LoRa module interface */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_UART_NUM           UART_NUM_1
#define LORA_UART_TX_PIN        (1)
#define LORA_UART_RX_PIN        (2)
#define LORA_UART_BAUD_RATE     (115200)
#define LORA_UART_BUF_SIZE      (1024)

#define LORA_DEFAULT_BAND       (915000000)  
#define LORA_DEFAULT_ADDRESS    (0)
#define LORA_MAX_PAYLOAD_LEN    (240)        
#define LORA_RESPONSE_TIMEOUT   (2000)       

typedef enum {
    LORA_SF5 = 5,
    LORA_SF6 = 6,
    LORA_SF7 = 7,
    LORA_SF8 = 8,
    LORA_SF9 = 9,
    LORA_SF10 = 10,
    LORA_SF11 = 11,
    LORA_SF12 = 12,
} lora_spreading_factor_t;

typedef enum {
    LORA_BW_7_8KHZ = 0,
    LORA_BW_10_4KHZ = 1,
    LORA_BW_15_6KHZ = 2,
    LORA_BW_20_8KHZ = 3,
    LORA_BW_31_25KHZ = 4,
    LORA_BW_41_7KHZ = 5,
    LORA_BW_62_5KHZ = 6,
    LORA_BW_125KHZ = 7,
    LORA_BW_250KHZ = 8,
    LORA_BW_500KHZ = 9,
} lora_bandwidth_t;

typedef enum {
    LORA_CR_4_5 = 1,
    LORA_CR_4_6 = 2,
    LORA_CR_4_7 = 3,
    LORA_CR_4_8 = 4,
} lora_coding_rate_t;

typedef enum {
    LORA_PREAMBLE_4 = 4,
    LORA_PREAMBLE_5 = 5,
    LORA_PREAMBLE_6 = 6,
    LORA_PREAMBLE_7 = 7,
    LORA_PREAMBLE_8 = 8,
    LORA_PREAMBLE_10 = 10,
    LORA_PREAMBLE_12 = 12,
    LORA_PREAMBLE_14 = 14,
    LORA_PREAMBLE_16 = 16,
    LORA_PREAMBLE_18 = 18,
    LORA_PREAMBLE_20 = 20,
} lora_preamble_t;

typedef struct {
    uint32_t frequency;                     
    lora_spreading_factor_t spreading_factor;
    lora_bandwidth_t bandwidth;
    lora_coding_rate_t coding_rate;
    lora_preamble_t preamble;
    uint16_t address;                       
    int8_t tx_power;                        
    char password[9];                       
} lora_config_t;

typedef struct {
    uint16_t sender_address;                
    uint8_t payload_length;                 
    uint8_t payload[LORA_MAX_PAYLOAD_LEN];  
    int16_t rssi;                           
    int8_t snr;                             
} lora_rx_msg_t;

typedef struct {
    uint16_t device_id;                     
    uint8_t device_type;                    
    char device_name[16];                   
    double latitude;                        
    double longitude;                       
    int16_t rssi;                           
    int8_t snr;                             
    bool valid;                             
} lora_gps_data_t;

typedef void (*lora_rx_callback_t)(const lora_rx_msg_t *msg, void *user_data);

bool lora_parse_gps_data(const lora_rx_msg_t *msg, lora_gps_data_t *gps_data);

esp_err_t lora_init(void);

esp_err_t lora_init_with_config(const lora_config_t *config);

esp_err_t lora_deinit(void);

esp_err_t lora_send_at_command(const char *cmd, char *response, size_t response_len, uint32_t timeout_ms);

esp_err_t lora_test(void);

esp_err_t lora_reset(void);

esp_err_t lora_set_band(uint32_t frequency);

esp_err_t lora_set_parameter(lora_spreading_factor_t sf, lora_bandwidth_t bw, 
                              lora_coding_rate_t cr, lora_preamble_t preamble);

esp_err_t lora_set_address(uint16_t address);

esp_err_t lora_set_network_id(uint8_t network_id);

esp_err_t lora_set_password(const char *password);

esp_err_t lora_set_tx_power(int8_t power);

esp_err_t lora_send(uint16_t dest_addr, const uint8_t *data, uint8_t len);

esp_err_t lora_send_string(uint16_t dest_addr, const char *str);

esp_err_t lora_send_string_with_ack(uint16_t dest_addr, const char *str, uint32_t ack_timeout_ms);

esp_err_t lora_register_rx_callback(lora_rx_callback_t callback, void *user_data);

esp_err_t lora_enter_power_save_mode(uint16_t rx_time, uint16_t low_speed_time);

esp_err_t lora_get_version(char *version, size_t len);

esp_err_t lora_get_signal_quality(int16_t *rssi, int8_t *snr);

#ifdef __cplusplus
}
#endif
