#include "pmu_axp2101.hpp"
#include <cstring>

static int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len);
static int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len);

static i2c_master_bus_handle_t g_pmu_bus = nullptr;  // Global bus handle used by callbacks
static i2c_master_dev_handle_t g_pmu_dev = nullptr;  // Kept for the life of the driver
static uint8_t g_pmu_dev_addr = 0;

/**
 * One device handle, created once.
 *
 * This file originally added a device to the bus and removed it again around
 * every register access, and split a register read into a write transaction
 * and a read transaction with the bus released in between. The AXP2101 shares
 * this bus with the touch controller, which is polled from the LVGL task while
 * the battery is polled from its own, so both were worth tightening up.
 */
static i2c_master_dev_handle_t pmu_device(uint8_t devAddr)
{
    if (g_pmu_bus == nullptr) return nullptr;
    if (g_pmu_dev != nullptr && g_pmu_dev_addr == devAddr) return g_pmu_dev;

    if (g_pmu_dev != nullptr) {
        i2c_master_bus_rm_device(g_pmu_dev);
        g_pmu_dev = nullptr;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = devAddr,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(g_pmu_bus, &cfg, &g_pmu_dev) != ESP_OK) {
        g_pmu_dev = nullptr;
        return nullptr;
    }
    g_pmu_dev_addr = devAddr;
    return g_pmu_dev;
}

AXP2101Driver::AXP2101Driver(i2c_master_bus_handle_t bus, uint8_t address)
    : _bus(bus), _addr(address) {}

esp_err_t AXP2101Driver::begin()
{
    g_pmu_bus = _bus;  // Store bus handle for use in callbacks

    if(_pmu.begin(_addr, pmu_register_read, pmu_register_write_byte)){

    }else{
         return ESP_FAIL;
    }



    _pmu.clearIrqStatus();

    _pmu.enableVbusVoltageMeasure();
    _pmu.enableBattVoltageMeasure();
    _pmu.enableSystemVoltageMeasure();
    _pmu.enableTemperatureMeasure();

    // It is necessary to disable the detection function of the TS pin on the board
    // without the battery temperature detection function, otherwise it will cause abnormal charging
    _pmu.disableTSPinMeasure();


    // Disable all interrupts
    // _pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    // Clear all interrupt flags
    // _pmu.clearIrqStatus();
    // Enable the required interrupt function
    // _pmu.enableIRQ(
    //     XPOWERS_AXP2101_BAT_INSERT_IRQ | XPOWERS_AXP2101_BAT_REMOVE_IRQ |    // BATTERY
    //     XPOWERS_AXP2101_VBUS_INSERT_IRQ | XPOWERS_AXP2101_VBUS_REMOVE_IRQ |  // VBUS
    //     XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ |     // POWER KEY
    //     XPOWERS_AXP2101_BAT_CHG_DONE_IRQ | XPOWERS_AXP2101_BAT_CHG_START_IRQ // CHARGE
    //     // XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ | XPOWERS_AXP2101_PKEY_POSITIVE_IRQ   |   //POWER KEY
    // );

    // Set the precharge charging current
    _pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
    // Set constant current charge current limit
    _pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_200MA);
    // Set stop charging termination current
    _pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);

    // Set charge cut-off voltage
    _pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V1);

    return ESP_OK;
}

bool AXP2101Driver::isCharging(){
    return _pmu.isCharging();
}

bool AXP2101Driver::isVbusIn(){
    return _pmu.isVbusIn();
}

bool AXP2101Driver::isBatteryConnected()
{
    return _pmu.isBatteryConnect();
}

int AXP2101Driver::getBatteryPercent()
{
    return _pmu.getBatteryPercent();
}

static int pmu_register_read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    if (len == 0 || data == nullptr) return -1;

    i2c_master_dev_handle_t dev = pmu_device(devAddr);
    if (dev == nullptr) return -1;

    /* One transaction with a repeated start, so nothing can come between the
     * register address and the bytes read back for it. */
    return i2c_master_transmit_receive(dev, &regAddr, 1, data, len, 100) == ESP_OK ? 0 : -1;
}

static int pmu_register_write_byte(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    if (!data || len == 0 || len > 127) return -1;

    i2c_master_dev_handle_t dev = pmu_device(devAddr);
    if (dev == nullptr) return -1;

    uint8_t buffer[128];
    buffer[0] = regAddr;
    memcpy(&buffer[1], data, len);

    return i2c_master_transmit(dev, buffer, len + 1, 100) == ESP_OK ? 0 : -1;
}
