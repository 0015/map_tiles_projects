/**
 * @file adc_bsp.h
 * @brief ADC Board Support Package header
 * 
 * Based on official Waveshare ESP32-S3-Touch-LCD-3.49 examples.
 */

#ifndef ADC_BSP_H
#define ADC_BSP_H

#ifdef __cplusplus
extern "C" {
#endif

void adc_bsp_init(void);
void adc_get_value(float *value, int *data);

#ifdef __cplusplus
}
#endif

#endif
