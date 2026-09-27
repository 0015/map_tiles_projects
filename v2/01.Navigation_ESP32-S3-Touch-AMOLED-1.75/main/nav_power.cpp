/**
 * Battery readout over the AXP2101, wrapped in a C API for the rest of the app.
 *
 * XPowersLib is C++, and everything else in this project is C; this file is the
 * only place the two meet.
 */

#include "nav_power.h"
#include "pmu_axp2101.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "nav_power";

static AXP2101Driver  *s_pmu;
static SemaphoreHandle_t s_lock;
static nav_power_state_t s_state = { false, false, false, false, -1 };
static TaskHandle_t   s_task;
static volatile bool  s_running;

/** Read the PMU and publish the result. Runs on the monitor task. */
static void sample(void)
{
    if (!s_pmu) return;

    nav_power_state_t s = {};
    s.present  = true;
    s.battery  = s_pmu->isBatteryConnected();
    s.charging = s_pmu->isCharging();
    s.vbus     = s_pmu->isVbusIn();
    s.percent  = s.battery ? s_pmu->getBatteryPercent() : -1;

    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_state = s;
        xSemaphoreGive(s_lock);
    }
}

static void monitor_task(void *arg)
{
    const uint32_t period_ms = (uint32_t)(uintptr_t)arg;

    while (s_running) {
        sample();
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

bool nav_power_init(void)
{
    if (s_pmu) return true;

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return false;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus is not up; call bsp_i2c_init() first");
        return false;
    }

    AXP2101Driver *pmu = new AXP2101Driver(bus, AXP2101_SLAVE_ADDRESS);
    if (pmu->begin() != ESP_OK) {
        ESP_LOGW(TAG, "No AXP2101 on the bus; running without a battery readout");
        delete pmu;
        return false;
    }

    s_pmu = pmu;
    sample();

    nav_power_state_t s;
    nav_power_get(&s);
    ESP_LOGI(TAG, "AXP2101 up: battery %s, %d%%, %s",
             s.battery ? "connected" : "absent", s.percent,
             s.charging ? "charging" : (s.vbus ? "on USB" : "on battery"));
    return true;
}

void nav_power_start_monitor(uint32_t period_ms)
{
    if (!s_pmu || s_task) return;
    if (period_ms < 1000) period_ms = 1000;

    s_running = true;
    if (xTaskCreate(monitor_task, "nav_power", 3072, (void *)(uintptr_t)period_ms,
                    2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the battery monitor task");
        s_running = false;
    }
}

void nav_power_stop_monitor(void)
{
    s_running = false;
}

void nav_power_get(nav_power_state_t *out)
{
    if (!out) return;

    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
        *out = s_state;
        xSemaphoreGive(s_lock);
        return;
    }

    /* The monitor task has the lock, or there is no PMU at all. Read it
     * anyway rather than reporting "no battery" for a frame: every field is
     * word-sized, so the worst a torn read can do is pair this second's
     * charging flag with last minute's percentage. */
    *out = s_state;
}
