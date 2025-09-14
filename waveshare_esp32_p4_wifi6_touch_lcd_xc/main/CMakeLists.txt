set(SOURCES
    main.cpp
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS .
    REQUIRES nvs_flash bsp_extra lvgl simple_map)