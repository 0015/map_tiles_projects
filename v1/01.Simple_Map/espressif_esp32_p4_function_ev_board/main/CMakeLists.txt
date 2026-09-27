set(SOURCES
    main.cpp
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS .
    REQUIRES nvs_flash bsp_extra esp32_p4_function_ev_board lvgl simple_map)