/**
 * @file main.cpp
 * @brief ESP32-S3 LoRa GPS tracker main application
 * 
 * Display initialization, LCD setup, touch interface, audio system, and button handling
 * are based on official Waveshare ESP32-S3-Touch-LCD-3.49 examples.
 * 
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"

#include "lvgl.h"
#include "esp_lcd_axs15231b.h"
#include "user_config.h"
#include "esp_io_expander_tca9554.h"
#include "driver/i2c_master.h"
#include "sdcard_bsp.h"
#include "i2c_bsp.h"
#include "lcd_bl_pwm_bsp.h"
#include "adc_bsp.h"
#include "button_bsp.h"

extern "C" {
#include "user_audio_bsp.h"
#include "codec_board.h"
#include "codec_init.h"
#include "esp_codec_dev.h"
}

#include "user_map.hpp"

#include "gps_lc76g.h"
#include "lora_rylr998.h"
#include "device_config.h"

static const char *TAG = "ESP32-S3";

extern const uint8_t alarm_pcm_start[] asm("_binary_TF022_pcm_start");
extern const uint8_t alarm_pcm_end[]   asm("_binary_TF022_pcm_end");

static SemaphoreHandle_t lvgl_mux = NULL;   
static SemaphoreHandle_t flush_done_semaphore = NULL; 
uint8_t *lvgl_dest = NULL;

static uint16_t *trans_buf_1;

/* LVGL UI objects */
static lv_obj_t *splash_screen = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *spinner = NULL;

static bool is_vbat_powered = false;

extern esp_io_expander_handle_t io_expander;

static bool lvgl_lock(int timeout_ms);
static void lvgl_unlock(void);

static void update_splash_status(const char *status)
{
    if (!status_label) {
        return;
    }
    
    if (lvgl_lock(100)) {
        lv_label_set_text(status_label, status);
        lvgl_unlock();
    }
    
    vTaskDelay(pdMS_TO_TICKS(300));
}

static void create_splash_screen(void)
{
    if (!lvgl_lock(500)) {
        return;
    }
    
    // Create splash screen container
    splash_screen = lv_obj_create(lv_screen_active());
    lv_obj_set_size(splash_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(splash_screen, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(splash_screen, 0, 0);
    lv_obj_set_style_radius(splash_screen, 0, 0);
    lv_obj_clear_flag(splash_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Left section: Title and system info
    lv_obj_t *left_container = lv_obj_create(splash_screen);
    lv_obj_set_size(left_container, 320, 172);
    lv_obj_set_pos(left_container, 0, 0);
    lv_obj_set_style_bg_color(left_container, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(left_container, 0, 0);
    lv_obj_set_style_radius(left_container, 0, 0);
    lv_obj_set_style_pad_all(left_container, 10, 0);
    lv_obj_clear_flag(left_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // Title label
    lv_obj_t *title_label = lv_label_create(left_container);
    lv_label_set_text(title_label, "ESP32-S3\nLoRa GPS");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x16f4d0), 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);
    
    // Info text
    lv_obj_t *info_label = lv_label_create(left_container);
    lv_label_set_text(info_label, "\n\n\n\nLC76G GNSS\nGPS/GLONASS");
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(info_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(info_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info_label, LV_ALIGN_CENTER, 0, 15);

    // Right section: Spinner and status
    lv_obj_t *right_container = lv_obj_create(splash_screen);
    lv_obj_set_size(right_container, 320, 172);
    lv_obj_set_pos(right_container, 320, 0);
    lv_obj_set_style_bg_color(right_container, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(right_container, 0, 0);
    lv_obj_set_style_radius(right_container, 0, 0);
    lv_obj_set_style_pad_all(right_container, 10, 0);
    lv_obj_clear_flag(right_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // Spinner
    spinner = lv_spinner_create(right_container);
    lv_obj_set_size(spinner, 60, 60);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x16f4d0), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);
    
    // Status label
    status_label = lv_label_create(right_container);
    lv_label_set_text(status_label, "Initializing...");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 45);
    
    lvgl_unlock();
}

static void gps_nmea_handler(const char *sentence, void *user_data)
{
    (void)sentence;
    (void)user_data;
}

static void gps_data_handler(const gps_data_t *data, void *user_data)
{
    (void)data;
    (void)user_data;
}

static void lora_rx_handler(const lora_rx_msg_t *msg, void *user_data)
{
    // Parse GPS data from received message
    lora_gps_data_t gps_data;
    if (lora_parse_gps_data(msg, &gps_data)) {
        double display_lat = gps_data.latitude;
        double display_lon = gps_data.longitude;
        
        // Update remote marker on map with translated coordinates (for display)
        if (lvgl_lock(100)) {
            UserMap::update_remote_marker(gps_data.device_id, gps_data.device_type, 
                                           gps_data.device_name, display_lat, display_lon);
            lvgl_unlock();
        }
    } else {
        // If not GPS data, print as raw string
        char rx_str[LORA_MAX_PAYLOAD_LEN + 1];
        memcpy(rx_str, msg->payload, msg->payload_length);
        rx_str[msg->payload_length] = '\0';
        ESP_LOGI(TAG, "📡 RX from Device %u: %s | RSSI=%d dBm, SNR=%d dB",
                 msg->sender_address, rx_str, msg->rssi, msg->snr);
    }
}

static void gps_map_update_task(void *pvParameters)
{
    gps_data_t gps_data;
    gps_data_t last_gps_data;
    memset(&last_gps_data, 0, sizeof(gps_data_t));
    bool map_loaded = false;
    
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    ESP_LOGI(TAG, "GPS map update task started, waiting for valid GPS data...");
    
    while (1) {
        esp_err_t get_result = gps_get_data(&gps_data);
        
        if (get_result == ESP_OK && gps_data.valid) {
            if (!map_loaded) {
                if (lvgl_lock(500)) {
                    if (splash_screen) {
                        lv_obj_delete(splash_screen);
                        splash_screen = NULL;
                        status_label = NULL;
                        spinner = NULL;
                    }
                    
                    if (!UserMap::init(lv_screen_active())) {
                        ESP_LOGE(TAG, "Failed to initialize map");
                        lvgl_unlock();
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        continue;
                    }
                    
                    UserMap::show_location(gps_data.latitude, gps_data.longitude, 18);
                    UserMap::center_map_on_gps();
                    
                    lvgl_unlock();
                    
                    map_loaded = true;
                    last_gps_data = gps_data;
                }
            } else {
                bool gps_changed = (gps_data.latitude != last_gps_data.latitude || 
                                   gps_data.longitude != last_gps_data.longitude);
                
                if (gps_changed) {
                    if (lvgl_lock(100)) {
                        UserMap::update_location(gps_data.latitude, gps_data.longitude);
                        lvgl_unlock();
                        last_gps_data = gps_data;
                    }
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void battery_monitor_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    float battery_voltage = 0.0f;
    
    while (1) {
        adc_get_value(&battery_voltage, NULL);
        
        if (lvgl_lock(100)) {
            UserMap::update_battery_voltage(battery_voltage);
            lvgl_unlock();
        }
        
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void alarm_monitor_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    esp_codec_dev_handle_t playback = get_playback_handle();
    if (!playback) {
        vTaskDelete(NULL);
        return;
    }
    
    size_t alarm_bytes_total = alarm_pcm_end - alarm_pcm_start;
    
    while (1) {
        double my_lat, my_lon;
        UserMap::get_current_location(&my_lat, &my_lon);
        
        bool should_alarm = false;
        int remote_count = UserMap::remote_marker_count;
        
        for (int i = 0; i < remote_count; i++) {
            auto* marker = &UserMap::remote_markers[i];
            if (marker && marker->latitude != 0.0 && marker->longitude != 0.0) {
                double distance_km = UserMap::calculate_distance(my_lat, my_lon, 
                                                                   marker->latitude, marker->longitude);
                double distance_ft = distance_km * 3280.84;
                float alarm_threshold = device_get_alarm_threshold();
                if (distance_ft > alarm_threshold) {
                    should_alarm = true;
                    ESP_LOGW(TAG, "ALARM: Device %u is %.1f ft away (threshold: %.1f ft)", 
                            marker->device_id, distance_ft, alarm_threshold);
                    break;
                }
            }
        }
        
        if (should_alarm) {
            esp_codec_dev_set_out_vol(playback, 100.0);
            
            esp_codec_dev_sample_info_t fs;
            memset(&fs, 0, sizeof(esp_codec_dev_sample_info_t));
            fs.sample_rate = 11025;
            fs.bits_per_sample = 16;
            fs.channel = 1;
            
            if (esp_codec_dev_open(playback, &fs) == ESP_CODEC_DEV_OK) {
                size_t bytes_write = 0;
                uint8_t *data_ptr = (uint8_t *)alarm_pcm_start;
                
                while (bytes_write < alarm_bytes_total) {
                    esp_codec_dev_write(playback, data_ptr, 256);
                    data_ptr += 256;
                    bytes_write += 256;
                }
                
                esp_codec_dev_close(playback);
            }
            
            // Wait 500ms before checking again
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            // No alarm needed, check every 2 seconds
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}

static void check_power_source_task(void *pvParameters)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_16),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    
    is_vbat_powered = gpio_get_level(GPIO_NUM_16) ? true : false;
    
    vTaskDelete(NULL);
}

static void power_button_monitor_task(void *pvParameters)
{
    while (1) {
        EventBits_t event = xEventGroupWaitBits(pwr_groups, set_bit_all, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        
        if (get_bit_button(event, 1)) {
            if (is_vbat_powered) {
                esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_6, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        else if (get_bit_button(event, 2)) {
            if (!is_vbat_powered) {
                is_vbat_powered = true;
            }
        }
    }
}

static void gps_broadcast_task(void *pvParameters)
{
    gps_data_t gps_data;
    vTaskDelay(pdMS_TO_TICKS(10000));
    
    while (1) {
        if (gps_get_data(&gps_data) == ESP_OK && gps_data.valid) {
            char gps_msg[80];
            const char *device_name = device_get_local_name();
            device_type_t device_type = device_get_local_type();
            snprintf(gps_msg, sizeof(gps_msg), "%s,%d,%.8f,%.8f", 
                    device_name, (int)device_type, gps_data.latitude, gps_data.longitude);
            
            lora_send_string(6, gps_msg);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10000)); // Send every 10 seconds
    }
}

#define LCD_BIT_PER_PIXEL 16
#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
#define BUFF_SIZE (EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * BYTES_PER_PIXEL)

#define LVGL_TICK_PERIOD_MS    5
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 10
#define LVGL_TASK_STACK_SIZE   (10 * 1024)
#define LVGL_TASK_PRIORITY     2


esp_io_expander_handle_t io_expander = NULL;

static void tca9554_init(void)
{
    i2c_master_bus_handle_t tca9554_i2c_bus_ = NULL;
  	ESP_ERROR_CHECK(i2c_master_get_bus_handle(0,&tca9554_i2c_bus_));
  	esp_io_expander_new_i2c_tca9554(tca9554_i2c_bus_, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &io_expander);
	esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_7 | IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_OUTPUT);
  	esp_io_expander_set_level(io_expander, IO_EXPANDER_PIN_NUM_7 | IO_EXPANDER_PIN_NUM_6, 1);
}

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = 
{
  	{0x11, (uint8_t []){0x00}, 0, 100},
    {0x29, (uint8_t []){0x00}, 0, 100},
};

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
  	BaseType_t high_task_awoken = pdFALSE;
  	xSemaphoreGiveFromISR(flush_done_semaphore, &high_task_awoken);
  	return false;
}

static void example_lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    lv_draw_sw_rgb565_swap(color_p, lv_area_get_width(area) * lv_area_get_height(area));
#if (Rotated == USER_DISP_ROT_90)
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);
    lv_area_t rotated_area;
    if(rotation != LV_DISPLAY_ROTATION_0)
    {
        lv_color_format_t cf = lv_display_get_color_format(disp);
        /*Calculate the position of the rotated area*/
        rotated_area = *area;
        lv_display_rotate_area(disp, &rotated_area);
        /*Calculate the source stride (bytes in a line) from the width of the area*/
        uint32_t src_stride = lv_draw_buf_width_to_stride(lv_area_get_width(area), cf);
        /*Calculate the stride of the destination (rotated) area too*/
        uint32_t dest_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&rotated_area), cf);
        /*Have a buffer to store the rotated area and perform the rotation*/
        
        int32_t src_w = lv_area_get_width(area);
        int32_t src_h = lv_area_get_height(area);
        lv_draw_sw_rotate(color_p, lvgl_dest, src_w, src_h, src_stride, dest_stride, rotation, cf);
        /*Use the rotated area and rotated buffer from now on*/
        area = &rotated_area;
    }

    const int flush_coun = (LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN);
    const int offgap = (EXAMPLE_LCD_V_RES / flush_coun);
    const int dmalen = (LVGL_DMA_BUFF_LEN / 2);
    int offsetx1 = 0;
    int offsety1 = 0;
    int offsetx2 = EXAMPLE_LCD_H_RES;
    int offsety2 = offgap;

    uint16_t *map = (uint16_t *)lvgl_dest;
    xSemaphoreGive(flush_done_semaphore);
    for(int i = 0; i<flush_coun; i++)
    {
        xSemaphoreTake(flush_done_semaphore,portMAX_DELAY);
        memcpy(trans_buf_1,map,LVGL_DMA_BUFF_LEN);
        esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2, offsety2, trans_buf_1);
        offsety1 += offgap;
        offsety2 += offgap;
        map += dmalen;
    }
    xSemaphoreTake(flush_done_semaphore,portMAX_DELAY);
    lv_disp_flush_ready(disp);
#else
    const int flush_coun = (LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN);
    const int offgap = (EXAMPLE_LCD_V_RES / flush_coun);
    const int dmalen = (LVGL_DMA_BUFF_LEN / 2);
    int offsetx1 = 0;
    int offsety1 = 0;
    int offsetx2 = EXAMPLE_LCD_H_RES;
    int offsety2 = offgap;

    uint16_t *map = (uint16_t *)color_p;
    xSemaphoreGive(flush_done_semaphore);
    for(int i = 0; i<flush_coun; i++)
    {
        xSemaphoreTake(flush_done_semaphore,portMAX_DELAY);
        memcpy(trans_buf_1,map,LVGL_DMA_BUFF_LEN);
        esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2, offsety2, trans_buf_1);
        offsety1 += offgap;
        offsety2 += offgap;
        map += dmalen;
    }
    xSemaphoreTake(flush_done_semaphore,portMAX_DELAY);
    lv_disp_flush_ready(disp);
#endif
}

static void TouchInputReadCallback(lv_indev_t * indev, lv_indev_data_t *indevData)
{
    uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e,0x0, 0x0, 0x0};
    uint8_t buff[32] = {0};
    ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write_read_dev(disp_touch_dev_handle,read_touchpad_cmd,11,buff,32));
    uint16_t pointX;
    uint16_t pointY;
    pointX = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
    pointY = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
    if (buff[1]>0 && buff[1]<5)
    {
        indevData->state = LV_INDEV_STATE_PRESSED;
        indevData->point.x = pointY;
        indevData->point.y = (EXAMPLE_LCD_V_RES-pointX);
    }
    else 
    {
        indevData->state = LV_INDEV_STATE_RELEASED;
    }
}

static void example_increase_lvgl_tick(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static bool lvgl_lock(int timeout_ms)
{
    assert(lvgl_mux && "bsp_display_start must be called first");

    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

static void lvgl_unlock(void)
{
    assert(lvgl_mux && "bsp_display_start must be called first");
    xSemaphoreGive(lvgl_mux);
}

static void example_lvgl_port_task(void *arg)
{
    uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
    for(;;)
    {
        // Lock the mutex due to the LVGL APIs are not thread-safe
        if (lvgl_lock(-1))
        {
            task_delay_ms = lv_timer_handler();
            // Release the mutex
            lvgl_unlock();
        }
        if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
        {
            task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
        }
        else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
        {
            task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

extern "C" void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize device configuration (ESP32-S3 = KID)
    device_config_init(DEVICE_TYPE_KID);
    ESP_LOGI(TAG, "Device configured as: %s (Type: %d)", 
             device_get_local_name(), device_get_local_type());

    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
    i2c_master_Init();
    tca9554_init();
    flush_done_semaphore = xSemaphoreCreateBinary();
    assert(flush_done_semaphore);
    
    ESP_LOGI(TAG, "Initialize SPI bus");
	gpio_config_t gpio_conf = {};
    gpio_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_conf.mode = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask = ((uint64_t)0x01<<EXAMPLE_PIN_NUM_LCD_RST);
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num =  EXAMPLE_PIN_NUM_LCD_PCLK;  
    buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;            
    buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;             
    buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
    buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
    buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));  
    
	ESP_LOGI(TAG, "Install panel IO");
	esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    
    esp_lcd_panel_io_spi_config_t io_config = {};
		io_config.cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS;                 
        io_config.dc_gpio_num = -1;          
        io_config.spi_mode = 3;              
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;    
        io_config.on_color_trans_done = example_notify_lvgl_flush_ready; 
        //io_config.user_ctx = &disp_drv,         
        io_config.lcd_cmd_bits = 32;         
        io_config.lcd_param_bits = 8;        
        io_config.flags.quad_mode = true;                         
	ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &panel_io));
    
	axs15231b_vendor_config_t vendor_config = {};
    vendor_config.flags.use_qspi_interface = 1;
    vendor_config.init_cmds = lcd_init_cmds;
    vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
    
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
    panel_config.vendor_config = &vendor_config;
    
    ESP_LOGI(TAG, "Install panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel));
    
	ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST,1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST,0));
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_PIN_NUM_LCD_RST,1));
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    sdcard_init();

    /*lvgl port*/
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    lv_display_t * disp = lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);  
    lv_display_set_flush_cb(disp, example_lvgl_flush_cb);                           
    
    uint8_t *buffer_1 = NULL;
    uint8_t *buffer_2 = NULL;
    buffer_1 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
    assert(buffer_1);
    buffer_2 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
    assert(buffer_2);
	trans_buf_1 = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN, MALLOC_CAP_DMA);
	assert(trans_buf_1);
    lv_display_set_buffers(disp, buffer_1, buffer_2, BUFF_SIZE, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_user_data(disp, panel);
#if (Rotated == USER_DISP_ROT_90)
    lvgl_dest = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_SPIRAM);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
#endif

    /*port indev*/
    lv_indev_t *touch_indev = NULL;
    touch_indev = lv_indev_create();
    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_indev, TouchInputReadCallback);

    esp_timer_create_args_t lvgl_tick_timer_args = {};
    lvgl_tick_timer_args.callback = &example_increase_lvgl_tick;
    lvgl_tick_timer_args.name = "lvgl_tick";
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    lvgl_mux = xSemaphoreCreateMutex(); //mutex semaphores
    assert(lvgl_mux);
    xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL,0);

    ESP_LOGI(TAG, "Initializing ADC for battery monitoring...");
    adc_bsp_init();
    
    ESP_LOGI(TAG, "Initializing audio system...");
    user_audio_bsp_init();
    audio_play_init();
    
    // Initialize button system for power button
    ESP_LOGI(TAG, "Initializing button system...");
    button_Init();
    
    xTaskCreate(check_power_source_task, "check_power", 2048, NULL, 3, NULL);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    xTaskCreate(battery_monitor_task, "battery_monitor", 3072, NULL, 2, NULL);
    
    xTaskCreate(alarm_monitor_task, "alarm_monitor", 4096, NULL, 3, NULL);
    
    xTaskCreate(power_button_monitor_task, "power_button", 3072, NULL, 2, NULL);

    create_splash_screen();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    update_splash_status("Initializing LoRa...");
    ESP_LOGI(TAG, "Initializing LoRa module...");
    esp_err_t lora_err = lora_init();
    if (lora_err == ESP_OK) {
        ESP_LOGI(TAG, "LoRa initialized successfully");
        update_splash_status("LoRa Ready");
        vTaskDelay(pdMS_TO_TICKS(500));
        
        lora_register_rx_callback(lora_rx_handler, NULL);
    } else {
        ESP_LOGE(TAG, "Failed to initialize LoRa module");
        update_splash_status("LoRa Init Failed!");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    update_splash_status("Initializing GPS...");

    ESP_LOGI(TAG, "Initializing GPS LC76G module...");
    ret = gps_init();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "GPS module initialized successfully!");
        update_splash_status("GPS Module Ready");
        vTaskDelay(pdMS_TO_TICKS(500));
        
        gps_register_nmea_callback(gps_nmea_handler, NULL);
        
        gps_register_data_callback(gps_data_handler, NULL);
        
        xTaskCreate(gps_map_update_task, "gps_map_update", 4096, NULL, 4, NULL);
        
        xTaskCreate(gps_broadcast_task, "gps_broadcast", 4096, NULL, 3, NULL);
        
        update_splash_status("Waiting for GPS Fix...");
        ESP_LOGI(TAG, "Waiting for GPS signal...");
    } else {
        ESP_LOGE(TAG, "Failed to initialize GPS module!");
        update_splash_status("GPS Init Failed!");
        
        if (lvgl_lock(-1)) {
            if (!UserMap::init(lv_screen_active())) {
                printf("Failed to initialize map\n");
                return;
            }
            
            UserMap::show_location(47.6175803, -122.1235667, 18);
            UserMap::center_map_on_gps();
            
            lvgl_unlock();
        }
    }

}