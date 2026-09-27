/* Device configuration implementation */

#include "device_config.h"
#include <string.h>

LV_IMAGE_DECLARE(icon_mom);
LV_IMAGE_DECLARE(icon_kid);

static float alarm_threshold_feet = 100.0f;

static const device_config_t device_configs[DEVICE_TYPE_COUNT] = {
    [DEVICE_TYPE_MOM] = {
        .type = DEVICE_TYPE_MOM,
        .name = "Mom",
        .icon = &icon_mom,
        .color = 0x56C0FA
    },
    [DEVICE_TYPE_KID] = {
        .type = DEVICE_TYPE_KID,
        .name = "Kid",
        .icon = &icon_kid,
        .color = 0xFF69B4
    }
};

static device_type_t local_device_type = DEVICE_TYPE_KID;

void device_config_init(device_type_t type) {
    if (type < DEVICE_TYPE_COUNT) {
        local_device_type = type;
    }
}

const device_config_t* device_get_config(device_type_t type) {
    if (type < DEVICE_TYPE_COUNT) {
        return &device_configs[type];
    }
    return &device_configs[0];
}

const device_config_t* device_get_local_config(void) {
    return &device_configs[local_device_type];
}

void device_set_local_type(device_type_t type) {
    if (type < DEVICE_TYPE_COUNT) {
        local_device_type = type;
    }
}

device_type_t device_get_local_type(void) {
    return local_device_type;
}

const char* device_get_local_name(void) {
    return device_configs[local_device_type].name;
}

float device_get_alarm_threshold(void) {
    return alarm_threshold_feet;
}
