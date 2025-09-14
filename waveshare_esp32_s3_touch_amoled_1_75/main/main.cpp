#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "simple_map.hpp"

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    bsp_i2c_init();

    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        printf("Failed to mount SD card, error: %s\n", esp_err_to_name(err));
    }

    esp_io_expander_handle_t expander = bsp_io_expander_init();
    if (expander == NULL) {
        printf("Failed to initialize IO expander\n");
        return;
    }

    esp_io_expander_set_dir(expander, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_7, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_7, 1);
    vTaskDelay(pdMS_TO_TICKS(500));

    lv_disp_t *disp = bsp_display_start();
    bsp_display_backlight_on();

    bsp_display_lock(0);

    // Initialize the simple map
    if (!SimpleMap::init(lv_screen_active())) {
        printf("Failed to initialize map\n");
        return;
    }

    // Show a location (example coordinates)
    SimpleMap::show_location(37.77490, -122.41942, 16);
    SimpleMap::center_map_on_gps();

    bsp_display_unlock();
}