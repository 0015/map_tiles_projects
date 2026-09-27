/* Device configuration for GPS tracking system */

#pragma once

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DEVICE_TYPE_MOM = 0,
    DEVICE_TYPE_KID = 1,
    DEVICE_TYPE_COUNT
} device_type_t;

typedef struct {
    device_type_t type;
    const char *name;
    const lv_image_dsc_t *icon;
    uint32_t color;
} device_config_t;

const device_config_t* device_get_config(device_type_t type);
const device_config_t* device_get_local_config(void);
void device_set_local_type(device_type_t type);
device_type_t device_get_local_type(void);
const char* device_get_local_name(void);
float device_get_alarm_threshold(void);
void device_config_init(device_type_t type);

#ifdef __cplusplus
}
#endif
