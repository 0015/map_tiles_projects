#ifndef ESP_GMF_INIT_H
#define ESP_GMF_INIT_H

#include <stdio.h>

/**
 * @brief  I2S mode
 */
typedef enum {
  ESP_GMF_APP_I2S_MODE_STD = 0,  /*!< Standard I2S mode */
  ESP_GMF_APP_I2S_MODE_TDM,      /*!< TDM mode */
  ESP_GMF_APP_I2S_MODE_PDM,      /*!< PDM mode */
  ESP_GMF_APP_I2S_MODE_NONE,     /*!< None mode */
} esp_gmf_app_i2s_mode_t;


/**
 * @brief  I2S driver information structure
 */
typedef struct {
  uint32_t                sample_rate;      /*!< The audio sample rate */
  uint8_t                 channel;          /*!< The audio channel number */
  uint8_t                 bits_per_sample;  /*!< The audio bits per sample */
  esp_gmf_app_i2s_mode_t  mode;             /*!< The I2S mode */
} esp_gmf_app_i2s_cfg_t;


/**
 * @brief  Codec information structure
 */
typedef struct {
  void                  *i2c_handle;   /*!<  User-initialized I2C handle; otherwise, set to NULL to let the `esp_gmf_app_setup_codec_dev` initialize it. */
  esp_gmf_app_i2s_cfg_t  play_info;    /*!< Audio information for playback. Currently,
                                            only ESP_GMF_APP_I2S_MODE_STD I2S mode is supported */
  esp_gmf_app_i2s_cfg_t  record_info;  /*!< Audio information for recording */
} esp_gmf_app_codec_info_t;

/**
 * @brief  Default audio information settings
 */
#define ESP_GMF_APP_I2S_INFO_DEFAULT() {        \
  .sample_rate     = 48000,                     \
  .channel         = 2,                         \
  .bits_per_sample = 16,                        \
  .mode            = ESP_GMF_APP_I2S_MODE_STD,  \
}


/**
 * @brief  Default codec information settings
 */
#define ESP_GMF_APP_CODEC_INFO_DEFAULT() {        \
  .i2c_handle  = NULL,                            \
  .play_info   = ESP_GMF_APP_I2S_INFO_DEFAULT(),  \
  .record_info = ESP_GMF_APP_I2S_INFO_DEFAULT(),  \
}



#endif
