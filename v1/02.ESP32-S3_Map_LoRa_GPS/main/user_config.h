/**
 * @file user_config.h
 * @brief Hardware configuration for Waveshare ESP32-S3-Touch-LCD-3.49
 * 
 * Pin definitions and display configuration based on official Waveshare examples.
 * 
 */

#ifndef USER_CONFIG_H
#define USER_CONFIG_H

#define SDSPI_HOST SPI2_HOST
#define LCD_HOST SPI3_HOST

#define Touch_SCL_NUM (GPIO_NUM_18)
#define Touch_SDA_NUM (GPIO_NUM_17)

#define ESP_SCL_NUM (GPIO_NUM_48)
#define ESP_SDA_NUM (GPIO_NUM_47)

#define EXAMPLE_LCD_H_RES              172
#define EXAMPLE_LCD_V_RES              640
#define LVGL_DMA_BUFF_LEN    (EXAMPLE_LCD_H_RES * 64 * 2)
#define LVGL_SPIRAM_BUFF_LEN (EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * 2)

#define USER_DISP_ROT_90    0
#define USER_DISP_ROT_NONO  0
#define Rotated USER_DISP_ROT_NONO

#define EXAMPLE_PIN_NUM_LCD_CS            (GPIO_NUM_9)
#define EXAMPLE_PIN_NUM_LCD_PCLK          (GPIO_NUM_10) 
#define EXAMPLE_PIN_NUM_LCD_DATA0         (GPIO_NUM_11)
#define EXAMPLE_PIN_NUM_LCD_DATA1         (GPIO_NUM_12)
#define EXAMPLE_PIN_NUM_LCD_DATA2         (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_LCD_DATA3         (GPIO_NUM_14)
#define EXAMPLE_PIN_NUM_LCD_RST           (GPIO_NUM_21)
#define EXAMPLE_PIN_NUM_BK_LIGHT          (GPIO_NUM_8)


#define EXAMPLE_PIN_NUM_TOUCH_ADDR        0x3b
#define EXAMPLE_PIN_NUM_TOUCH_RST         (-1)
#define EXAMPLE_PIN_NUM_TOUCH_INT         (-1)

#define EXAMPLE_LVGL_TICK_PERIOD_MS    5
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 5



#define EXAMPLE_RTC_ADDR 0x51

#define EXAMPLE_IMU_ADDR 0x6b

#endif