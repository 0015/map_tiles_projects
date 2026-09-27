idf_component_register(
    SRCS main.c
         nav_engine.c
         nav_ui.c
         nav_routes.c
         nav_maps.c
         nav_power.cpp
         pmu_axp2101.cpp
         gps_source.c
         gps_lc76g.c
         compass_source.c
         qmi8658.c
    INCLUDE_DIRS .
    REQUIRES nvs_flash lvgl map_tiles XPowersLib driver esp_timer esp_driver_uart
)

# XPowersLib and the BSP both initialise structs field by field.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
