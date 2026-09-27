# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The vendor demo this project replaced shipped an audio BSP (bsp_extra) and a
# full LVGL checkout under components/. Both were removed rather than kept:
# bsp_extra drags in chmorgan/esp-audio-player, which includes
# "driver/i2s_std.h" while declaring only REQUIRES driver - that resolves up to
# ESP-IDF 5.5.x and fails from 6.0 on - and a local lvgl component would shadow
# the registry copy that map_tiles is built against.
#
# To add voice prompts, take bsp_extra from the Waveshare BSP repository and put
# it under components/ - and on ESP-IDF 6.x add esp_driver_i2s to its REQUIRES.

add_compile_options(-Wno-format)
project(navigation_s3_round)
