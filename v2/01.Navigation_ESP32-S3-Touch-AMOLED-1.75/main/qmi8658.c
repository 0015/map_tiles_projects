#include "qmi8658.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "qmi8658";

#define REG_WHO_AM_I        0x00
#define REG_REVISION        0x01
#define REG_CTRL1           0x02    /* serial interface */
#define REG_CTRL2           0x03    /* accelerometer range and rate */
#define REG_CTRL3           0x04    /* gyroscope range and rate */
#define REG_CTRL5           0x06    /* low-pass filters */
#define REG_CTRL7           0x08    /* sensor enables */
#define REG_AX_L            0x35    /* AX AY AZ GX GY GZ follow, 16 bits each */
#define REG_RESET_DONE      0x4D    /* reads RESET_DONE once a soft reset is over */
#define REG_RESET           0x60

#define WHO_AM_I            0x05
#define RESET_CMD           0xB0
#define RESET_DONE          0x80

/* Address auto-increment, so the twelve data bytes come in one read; little
 * endian; internal oscillator on. Written outright rather than bit by bit,
 * because the reset default of the endianness bit differs between datasheet
 * revisions. */
#define CTRL1_VALUE         0x40

/* Four g is plenty for finding down. 512 dps is past anything a vehicle does
 * and most of what a hand does, at 0.016 dps a count. */
#define ACC_FS_4G           (0x1 << 4)
#define GYR_FS_512DPS       (0x5 << 4)
#define ACC_SCALE_G         (4.0f / 32768.0f)
#define GYR_SCALE_DPS       (512.0f / 32768.0f)

/* 112 Hz. With both sensors on, the accelerometer runs at the gyroscope's
 * rate, so the two are given the same code. */
#define ODR_112HZ           0x6

/* Both low-pass filters on, at 13.37 % of the output rate: about 15 Hz. */
#define CTRL5_VALUE         ((0x3 << 5) | (1 << 4) | (0x3 << 1) | (1 << 0))

#define CTRL7_ACC_GYR       0x03

#define RESET_MS            15      /* the datasheet's figure */
#define RESET_POLL_MS       100     /* how long to wait for it past that */
#define STARTUP_MS          100     /* gyroscope turn-on to first good sample */

#define I2C_TIMEOUT_MS      50

struct qmi8658_dev_t {
    i2c_master_dev_handle_t i2c;
};

static esp_err_t rd(qmi8658_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev->i2c, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

static esp_err_t wr(qmi8658_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(dev->i2c, b, sizeof(b), I2C_TIMEOUT_MS);
}

static esp_err_t configure(qmi8658_handle_t dev)
{
    /* From a known state, whatever the last boot left behind. The chip may not
     * answer while it resets, so a failed poll just means "not yet". */
    ESP_RETURN_ON_ERROR(wr(dev, REG_RESET, RESET_CMD), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(RESET_MS));

    uint8_t done = 0;
    for (int waited = 0; waited < RESET_POLL_MS; waited += 5) {
        if (rd(dev, REG_RESET_DONE, &done, 1) == ESP_OK && done == RESET_DONE) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (done != RESET_DONE) {
        ESP_LOGW(TAG, "Reset did not report done (0x%02X); configuring anyway", done);
    }

    ESP_RETURN_ON_ERROR(wr(dev, REG_CTRL1, CTRL1_VALUE), TAG, "interface");
    ESP_RETURN_ON_ERROR(wr(dev, REG_CTRL7, 0x00), TAG, "disable");
    ESP_RETURN_ON_ERROR(wr(dev, REG_CTRL2, ACC_FS_4G | ODR_112HZ), TAG, "accelerometer");
    ESP_RETURN_ON_ERROR(wr(dev, REG_CTRL3, GYR_FS_512DPS | ODR_112HZ), TAG, "gyroscope");
    ESP_RETURN_ON_ERROR(wr(dev, REG_CTRL5, CTRL5_VALUE), TAG, "filters");
    ESP_RETURN_ON_ERROR(wr(dev, REG_CTRL7, CTRL7_ACC_GYR), TAG, "enable");

    vTaskDelay(pdMS_TO_TICKS(STARTUP_MS));
    return ESP_OK;
}

esp_err_t qmi8658_init(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t scl_speed_hz,
                       qmi8658_handle_t *out)
{
    ESP_RETURN_ON_FALSE(bus && out, ESP_ERR_INVALID_ARG, TAG, "no bus");
    *out = NULL;

    /* A probe first, so a chip that is not there is ESP_ERR_NOT_FOUND rather
     * than a failed read further in. */
    esp_err_t err = i2c_master_probe(bus, addr, I2C_TIMEOUT_MS);
    if (err != ESP_OK) return (err == ESP_ERR_TIMEOUT) ? err : ESP_ERR_NOT_FOUND;

    struct qmi8658_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) return ESP_ERR_NO_MEM;

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = scl_speed_hz,
    };
    err = i2c_master_bus_add_device(bus, &cfg, &dev->i2c);
    if (err != ESP_OK) {
        free(dev);
        return err;
    }

    uint8_t id = 0;
    err = rd(dev, REG_WHO_AM_I, &id, 1);
    if (err == ESP_OK && id != WHO_AM_I) {
        ESP_LOGW(TAG, "0x%02X answers but WHO_AM_I is 0x%02X, not 0x%02X", addr, id, WHO_AM_I);
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) err = configure(dev);
    if (err != ESP_OK) {
        qmi8658_deinit(dev);
        return err;
    }

    uint8_t rev = 0;
    rd(dev, REG_REVISION, &rev, 1);
    ESP_LOGI(TAG, "QMI8658 at 0x%02X, revision 0x%02X", addr, rev);

    *out = dev;
    return ESP_OK;
}

void qmi8658_deinit(qmi8658_handle_t dev)
{
    if (!dev) return;
    if (dev->i2c) i2c_master_bus_rm_device(dev->i2c);
    free(dev);
}

esp_err_t qmi8658_read(qmi8658_handle_t dev, qmi8658_sample_t *out)
{
    uint8_t b[12];
    esp_err_t err = rd(dev, REG_AX_L, b, sizeof(b));
    if (err != ESP_OK) return err;

    for (int i = 0; i < 3; i++) {
        out->acc_g[i]   = (float)(int16_t)(b[2 * i]     | (b[2 * i + 1] << 8)) * ACC_SCALE_G;
        out->gyr_dps[i] = (float)(int16_t)(b[6 + 2 * i] | (b[7 + 2 * i] << 8)) * GYR_SCALE_DPS;
    }
    return ESP_OK;
}
