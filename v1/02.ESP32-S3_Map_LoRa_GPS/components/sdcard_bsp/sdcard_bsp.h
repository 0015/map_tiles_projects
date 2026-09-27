/**
 * @file sdcard_bsp.h
 * @brief SD Card Board Support Package header
 * 
 * Based on official Waveshare ESP32-S3-Touch-LCD-3.49 examples.
 */

#ifndef SDCARD_BSP_H
#define SDCARD_BSP_H

#ifdef __cplusplus
extern "C" {
#endif

void sdcard_init(void);
float sdcard_GetValue(void);
esp_err_t s_example_read_file(const char *path,char *pxbuf,uint32_t *outLen);
esp_err_t s_example_write_file(const char *path, char *data);

#ifdef __cplusplus
}
#endif

#endif