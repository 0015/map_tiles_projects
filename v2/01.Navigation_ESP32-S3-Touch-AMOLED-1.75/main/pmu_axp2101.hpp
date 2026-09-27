#pragma once

#include "esp_log.h"
#include "XPowersLib.h"
#include "driver/i2c_master.h"

class AXP2101Driver {
public:
    AXP2101Driver(i2c_master_bus_handle_t bus, uint8_t address = AXP2101_SLAVE_ADDRESS);
    esp_err_t begin();
    bool isBatteryConnected();
    bool isCharging();
    bool isVbusIn();
    int getBatteryPercent();

private:
    i2c_master_bus_handle_t _bus;
    uint8_t _addr;
    XPowersAXP2101 _pmu;
};
