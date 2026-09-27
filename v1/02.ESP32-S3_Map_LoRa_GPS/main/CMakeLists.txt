idf_component_register(
    SRCS main.cpp user_map.cpp gps_lc76g.c lora_rylr998.c icon_mom.c icon_kid.c device_config.c
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer i2c_bsp sdcard_bsp lcd_bl_pwm_bsp adc_bsp audio_bsp codec_board button_bsp esp_lcd_axs15231b esp_io_expander_tca9554 lvgl
    )
