#include "compass_source.h"
#include "nav_config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "map_geo.h"

#define STALE_MS    1000

/* Written by the compass task and the GPS task, read from the LVGL thread. A
 * spinlock rather than a mutex: every copy is a few bytes, and nobody may wait. */
static portMUX_TYPE  s_lock = portMUX_INITIALIZER_UNLOCKED;
static nav_compass_t s_latest;

/** The newest GPS report, left by the GPS task for the compass task. */
typedef struct {
    bool     valid;
    double   speed_mps;
    double   course_deg;
    uint32_t at_ms;
    uint32_t seq;
} course_t;

static course_t s_course;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool compass_source_get(nav_compass_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_latest;
    portEXIT_CRITICAL(&s_lock);

    if (out->valid && now_ms() - out->timestamp_ms > STALE_MS) out->valid = false;
    return out->valid;
}

void compass_source_feed_course(bool valid, double speed_mps, double course_deg)
{
    uint32_t t = now_ms();

    portENTER_CRITICAL(&s_lock);
    s_course.valid      = valid;
    s_course.speed_mps  = speed_mps;
    s_course.course_deg = course_deg;
    s_course.at_ms      = t;
    s_course.seq++;
    portEXIT_CRITICAL(&s_lock);
}

#if NAV_COMPASS_ENABLED

#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "qmi8658.h"

static const char *TAG = "compass";

#define RETRY_MS            3000
#define MAX_READ_FAILS      10

/* The longest gap between two samples integrated as one step. Past this a turn
 * has been missed rather than measured, and is booked as error instead. */
#define MAX_STEP_S          0.25f

/* The board straps it high. The other address is tried as well, in case a
 * board straps it low; the configured one goes first. */
static const uint8_t s_addrs[] = { NAV_COMPASS_I2C_ADDR, NAV_COMPASS_I2C_ADDR ^ 0x01 };

static TaskHandle_t s_task;

/* ----------------------------------------------------------------- the model
 *
 * How the gyroscope goes wrong, and how fast. These describe the part rather
 * than the ride, so they live here; the settings worth changing are in
 * nav_config.h.
 */

/* Stillness is judged over one-second windows. Still means the rate barely
 * varies - hand tremor alone is several times this - and neither does the
 * accelerometer. */
#define WINDOW_SAMPLES          NAV_COMPASS_SAMPLE_HZ
#define STILL_GYRO_STD_DPS      0.3f
#define STILL_ACC_STD_G         0.02f

/* A slow, smooth bend is steady too, and learning it as bias would bend every
 * straight after it. Once the bias has been measured, a window whose average
 * rate sits this far from it is a turn, however steady. */
#define STILL_MAX_RATE_DPS      1.0f

/* A car on a smooth road can pass both tests above. A fresh GPS report saying
 * it is moving overrides them. */
#define STILL_GPS_FRESH_MS      2000
#define STILL_GPS_MAX_MPS       0.5

/* How much of each still window's average goes into the bias. */
#define BIAS_BLEND              0.2f

/* Down comes from the accelerometer, which also feels braking and bumps. So
 * the up vector is carried through turns by the gyroscope and only pulled
 * toward the accelerometer slowly, and only while that reads close to 1 g. */
#define GRAVITY_TAU_S           1.0f
#define GRAVITY_ACCEPT_G        0.15f

/* How fast the error estimate grows while the device is moving: bias left
 * over after measuring it (or all of it, before), and a share of every degree
 * turned for scale error. Nothing grows while it is still. */
#define DRIFT_DEG_PER_S         (2.0f / 60.0f)
#define DRIFT_COLD_DEG_PER_S    (20.0f / 60.0f)
#define SCALE_ERROR             0.03f

/* One course at NAV_COMPASS_ALIGN_MIN_SPEED_MPS is good to about this. The
 * receiver's velocity error is roughly fixed and the course is its angle, so
 * faster is proportionally better, down to COURSE_BEST_DEG. */
#define COURSE_NOISE_DEG        10.0f
#define COURSE_BEST_DEG         2.0f

/* The error estimate never drops below this. Travel and facing differ a little
 * in every lane change, so the heading keeps listening to the course rather
 * than deciding it knows better. */
#define ERROR_FLOOR_DEG         3.0f

/* The course lags the turn it describes, so reports taken while turning faster
 * than this are skipped, as are ones the gyroscope has already moved on from. */
#define ALIGN_MAX_TURN_DPS      8.0f
#define ALIGN_MAX_AGE_MS        500
#define TURN_RATE_TAU_S         0.5f

/* The learned bias is kept across power cycles, so a board that boots on the
 * move does not start from nothing. Written only when it has moved, and at
 * most this often, because every write wears the flash. */
#define NVS_NAMESPACE           "compass"
#define NVS_KEY                 "qmi_bias"
#define BIAS_SAVE_MIN_MS        (10 * 60 * 1000)
#define BIAS_SAVE_DELTA_DPS     0.05f
#define BIAS_SANE_DPS           20.0f

/* Gyroscope zero-rate offset, per chip axis. Kept when the sensor is lost: it
 * is the same chip when it comes back. */
static float    s_bias[3];
static bool     s_bias_measured;        /* this session, from a still window */
static float    s_bias_saved[3];
static bool     s_bias_have_saved;      /* s_bias_saved is what NVS holds */
static uint32_t s_bias_saved_ms;        /* 0 until written this session */

/** Everything that starts over when the sensor is found again. */
typedef struct {
    float    up[3];                     /* unit vector toward the sky, chip axes */
    bool     up_known;

    double   heading_deg;
    float    error_deg;
    bool     aligned;                   /* north has been set, and not given up on */
    bool     trusted;                   /* error within NAV_COMPASS_TRUST_DEG */
    float    turn_dps;                  /* smoothed turn rate, for the alignment gate */
    uint32_t course_seq;

    /* The stillness window being gathered */
    int      n;
    float    g_sum[3], g_sq[3];
    float    a_sum[3], a_sq[3];
    double   win_turn_deg;              /* what the window added to the heading */
    float    win_abs_turn_deg;
    float    win_s;
} tracker_t;

static void publish(const nav_compass_t *c)
{
    portENTER_CRITICAL(&s_lock);
    s_latest = *c;
    portEXIT_CRITICAL(&s_lock);
}

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* ------------------------------------------------------------------- bias */

static void load_bias(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    float  b[3];
    size_t len = sizeof(b);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, b, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(b)) return;

    for (int i = 0; i < 3; i++) {
        if (!(fabsf(b[i]) < BIAS_SANE_DPS)) return;     /* NaN fails this too */
    }
    memcpy(s_bias, b, sizeof(b));
    memcpy(s_bias_saved, b, sizeof(b));
    s_bias_have_saved = true;
}

static void save_bias(void)
{
    uint32_t t = now_ms();
    if (s_bias_saved_ms && t - s_bias_saved_ms < BIAS_SAVE_MIN_MS) return;

    if (s_bias_have_saved) {
        float moved = 0;
        for (int i = 0; i < 3; i++) moved = fmaxf(moved, fabsf(s_bias[i] - s_bias_saved[i]));
        if (moved < BIAS_SAVE_DELTA_DPS) return;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, s_bias, sizeof(s_bias));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }

    /* Successful or not, not again for a while: a write that fails should not
     * be retried every second. */
    s_bias_saved_ms = t ? t : 1;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not save the gyroscope bias: %s", esp_err_to_name(err));
        return;
    }
    memcpy(s_bias_saved, s_bias, sizeof(s_bias));
    s_bias_have_saved = true;
}

static void learn_bias(const float mean[3])
{
    if (!s_bias_measured) {
        memcpy(s_bias, mean, sizeof(s_bias));
        s_bias_measured = true;
        ESP_LOGI(TAG, "Gyroscope bias measured: %.2f %.2f %.2f dps", mean[0], mean[1], mean[2]);
    } else {
        for (int i = 0; i < 3; i++) s_bias[i] += (mean[i] - s_bias[i]) * BIAS_BLEND;
    }
    save_bias();
}

/* ---------------------------------------------------------------- heading */

/** Keep "up" current: turned by the gyroscope, trimmed by the accelerometer. */
static void track_up(tracker_t *t, const float acc[3], const float w_dps[3], float dt)
{
    float an = sqrtf(dot3(acc, acc));

    if (!t->up_known) {
        if (an < 0.5f) return;          /* a read of zeros, or falling */
        for (int i = 0; i < 3; i++) t->up[i] = acc[i] / an;
        t->up_known = true;
        return;
    }

    /* A direction fixed in the world turns the other way in the chip's axes:
     * du/dt = u x w. */
    const float k = (float)M_PI / 180.0f * dt;
    const float w[3] = { w_dps[0] * k, w_dps[1] * k, w_dps[2] * k };
    float u[3] = {
        t->up[0] + (t->up[1] * w[2] - t->up[2] * w[1]),
        t->up[1] + (t->up[2] * w[0] - t->up[0] * w[2]),
        t->up[2] + (t->up[0] * w[1] - t->up[1] * w[0]),
    };

    if (fabsf(an - 1.0f) < GRAVITY_ACCEPT_G) {
        float g = fminf(dt / GRAVITY_TAU_S, 1.0f);
        for (int i = 0; i < 3; i++) u[i] += (acc[i] / an - u[i]) * g;
    }

    float un = sqrtf(dot3(u, u));
    if (un > 0.0f) {
        for (int i = 0; i < 3; i++) t->up[i] = u[i] / un;
    }
}

/** Move the heading on by one sample, and add the sample to the window. */
static void step(tracker_t *t, const qmi8658_sample_t *s, float dt)
{
    float w[3];
    for (int i = 0; i < 3; i++) w[i] = s->gyr_dps[i] - s_bias[i];

    track_up(t, s->acc_g, w, dt);

    if (t->up_known) {
        /* The turn about the vertical, whichever way up the chip is mounted.
         * Positive about "up" is anticlockwise seen from above; a heading
         * counts the other way. */
        float  yaw_dps = -dot3(w, t->up);
        double turn    = (double)yaw_dps * dt;

        t->heading_deg       = map_geo_normalize_deg(t->heading_deg + turn);
        t->win_turn_deg     += turn;
        t->win_abs_turn_deg += fabsf((float)turn);
        t->turn_dps         += (fabsf(yaw_dps) - t->turn_dps) * fminf(dt / TURN_RATE_TAU_S, 1.0f);
    }

    for (int i = 0; i < 3; i++) {
        t->g_sum[i] += s->gyr_dps[i];
        t->g_sq[i]  += s->gyr_dps[i] * s->gyr_dps[i];
        t->a_sum[i] += s->acc_g[i];
        t->a_sq[i]  += s->acc_g[i] * s->acc_g[i];
    }
    t->win_s += dt;
    t->n++;
}

/**
 * End a stillness window.
 *
 * In a still one nothing turned, so whatever it added to the heading was bias:
 * that is taken back out, and the bias learns from it. Any other window adds
 * to the error estimate instead.
 */
static void close_window(tracker_t *t, bool gps_moving)
{
    const float n = (float)t->n;
    float mean[3];
    float g_var = 0, a_var = 0, off = 0;

    for (int i = 0; i < 3; i++) {
        mean[i] = t->g_sum[i] / n;
        g_var  += t->g_sq[i] / n - mean[i] * mean[i];

        float am = t->a_sum[i] / n;
        a_var  += t->a_sq[i] / n - am * am;

        float d = mean[i] - s_bias[i];
        off    += d * d;
    }

    bool still = !gps_moving &&
                 g_var < STILL_GYRO_STD_DPS * STILL_GYRO_STD_DPS &&
                 a_var < STILL_ACC_STD_G * STILL_ACC_STD_G &&
                 (!s_bias_measured || off < STILL_MAX_RATE_DPS * STILL_MAX_RATE_DPS);

    if (still) {
        t->heading_deg = map_geo_normalize_deg(t->heading_deg - t->win_turn_deg);
        learn_bias(mean);
    } else if (t->aligned) {
        t->error_deg += (s_bias_measured ? DRIFT_DEG_PER_S : DRIFT_COLD_DEG_PER_S) * t->win_s +
                        SCALE_ERROR * t->win_abs_turn_deg;
    }

    t->n = 0;
    memset(t->g_sum, 0, sizeof(t->g_sum));
    memset(t->g_sq,  0, sizeof(t->g_sq));
    memset(t->a_sum, 0, sizeof(t->a_sum));
    memset(t->a_sq,  0, sizeof(t->a_sq));
    t->win_turn_deg     = 0;
    t->win_abs_turn_deg = 0;
    t->win_s            = 0;
}

/** Set north from the GPS course, if the newest report is one to go by. */
static void align(tracker_t *t, const course_t *c, uint32_t now)
{
    if (c->seq == t->course_seq) return;
    t->course_seq = c->seq;

    if (!c->valid || c->speed_mps < NAV_COMPASS_ALIGN_MIN_SPEED_MPS) return;
    if (now - c->at_ms > ALIGN_MAX_AGE_MS) return;
    if (!t->up_known || t->turn_dps > ALIGN_MAX_TURN_DPS) return;

    double target = map_geo_normalize_deg(c->course_deg + NAV_COMPASS_MOUNT_OFFSET_DEG);
    float  noise  = fmaxf(COURSE_NOISE_DEG *
                          (float)(NAV_COMPASS_ALIGN_MIN_SPEED_MPS / c->speed_mps),
                          COURSE_BEST_DEG);

    if (!t->aligned) {
        t->heading_deg = target;
        t->error_deg   = noise;
        t->aligned     = true;
        t->trusted     = noise <= NAV_COMPASS_TRUST_DEG;
        ESP_LOGI(TAG, "North set from the GPS course: heading %.0f at %.1f m/s",
                 target, c->speed_mps);
        return;
    }

    /* Each side weighted by how far it is to be believed, as a one-state
     * Kalman filter does it: a heading that has been drifting takes most of
     * the course, a fresh one only a little of each noisy report. */
    float e2 = t->error_deg * t->error_deg;
    float n2 = noise * noise;
    float k  = e2 / (e2 + n2);

    t->heading_deg = map_geo_normalize_deg(t->heading_deg +
                                           k * map_geo_angle_diff_deg(t->heading_deg, target));
    t->error_deg   = fmaxf(sqrtf(e2 * n2 / (e2 + n2)), ERROR_FLOOR_DEG);
}

/** Track the heading until the sensor stops answering. */
static void run_until_lost(qmi8658_handle_t dev)
{
    const TickType_t period = pdMS_TO_TICKS(1000 / NAV_COMPASS_SAMPLE_HZ);
    TickType_t wake = xTaskGetTickCount();

    tracker_t t = { 0 };
    int64_t   last_us = esp_timer_get_time();
    int       fails = 0;

    while (fails < MAX_READ_FAILS) {
        vTaskDelayUntil(&wake, period);

        qmi8658_sample_t s;
        if (qmi8658_read(dev, &s) != ESP_OK) {
            fails++;
            continue;
        }
        fails = 0;

        int64_t now_us = esp_timer_get_time();
        float   gap_s  = (float)(now_us - last_us) / 1e6f;
        float   dt     = fminf(gap_s, MAX_STEP_S);
        last_us = now_us;

        /* A turn that happened while the bus was busy was not seen. Assume it
         * went on at the rate it had, and count that as error. */
        if (t.aligned && gap_s > dt) t.error_deg += (gap_s - dt) * t.turn_dps;

        course_t c;
        portENTER_CRITICAL(&s_lock);
        c = s_course;
        portEXIT_CRITICAL(&s_lock);
        uint32_t now = now_ms();

        step(&t, &s, dt);
        align(&t, &c, now);

        if (t.n >= WINDOW_SAMPLES) {
            bool gps_moving = c.valid && c.speed_mps > STILL_GPS_MAX_MPS &&
                              now - c.at_ms < STILL_GPS_FRESH_MS;
            close_window(&t, gps_moving);
        }

        if (t.aligned && t.error_deg > NAV_COMPASS_GIVE_UP_DEG) {
            t.aligned = false;
            t.trusted = false;
            ESP_LOGW(TAG, "Heading may be %.0f deg off since the GPS last set it; the dial "
                          "hides until a course sets it again", t.error_deg);
        }

        bool trusted = t.aligned && t.error_deg <= NAV_COMPASS_TRUST_DEG;
        if (t.aligned && trusted != t.trusted) {
            if (trusted) {
                ESP_LOGI(TAG, "Heading set again from the GPS course");
            } else {
                ESP_LOGI(TAG, "Heading uncertain, may be %.0f deg off; the dial fades "
                              "until a GPS course sets it again", t.error_deg);
            }
        }
        t.trusted = trusted;

        nav_compass_t out = {
            .heading_deg  = (float)t.heading_deg,
            .error_deg    = t.error_deg,
            .valid        = t.aligned,
            .timestamp_ms = now,
        };
        publish(&out);
    }
}

/**
 * Log every address that answers on the bus.
 *
 * The QMI8658 is soldered to the board, so it going missing is a fault rather
 * than a configuration. This tells a chip that does not answer apart from a
 * bus that is not working: the touch controller (0x5A) and the PMU (0x34)
 * answer on it whatever state the IMU is in.
 */
static void log_bus_devices(i2c_master_bus_handle_t bus)
{
    char   list[96] = "";
    size_t n = 0;

    for (uint8_t a = 0x08; a < 0x78; a++) {
        esp_err_t err = i2c_master_probe(bus, a, 20);
        if (err == ESP_ERR_TIMEOUT) {
            /* SDA or SCL held low. Every other probe would time out too. */
            ESP_LOGE(TAG, "The I2C bus is stuck - check for a short on SDA or SCL");
            return;
        }
        if (err == ESP_OK && n < sizeof(list)) {
            n += snprintf(list + n, sizeof(list) - n, " 0x%02X", a);
        }
    }
    ESP_LOGW(TAG, "Answering on the bus:%s", n ? list : " nothing");
}

static void compass_task(void *arg)
{
    (void)arg;

    /* bsp_i2c_init() has already run in app_main. */
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "The BSP's I2C bus is not up; call bsp_i2c_init() first");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    load_bias();

    bool reported_missing = false;

    for (;;) {
        qmi8658_handle_t dev  = NULL;
        esp_err_t        err  = ESP_ERR_NOT_FOUND;
        for (size_t i = 0; i < sizeof(s_addrs) && err != ESP_OK; i++) {
            err = qmi8658_init(bus, s_addrs[i], CONFIG_BSP_I2C_CLK_SPEED_HZ, &dev);
        }
        if (err != ESP_OK) {
            /* Keep looking, but say so only once. */
            if (!reported_missing) {
                ESP_LOGW(TAG, "No QMI8658 at 0x%02X or 0x%02X (%s); the compass stays "
                              "hidden until one answers",
                         s_addrs[0], s_addrs[1], esp_err_to_name(err));
                log_bus_devices(bus);
                reported_missing = true;
            }
            vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
            continue;
        }

        reported_missing = false;
        ESP_LOGI(TAG, "Gyroscope bias %s. The dial appears once a GPS course at %.1f m/s "
                      "or more has set north",
                 s_bias_measured   ? "known" :
                 s_bias_have_saved ? "restored" :
                                     "not measured yet - it is, the first time the board is still",
                 NAV_COMPASS_ALIGN_MIN_SPEED_MPS);

        run_until_lost(dev);
        qmi8658_deinit(dev);

        nav_compass_t lost = { .valid = false, .timestamp_ms = now_ms() };
        publish(&lost);
        ESP_LOGW(TAG, "QMI8658 stopped answering; looking for it again");
    }
}

bool compass_source_start(void)
{
    if (s_task) return true;

    if (xTaskCreate(compass_task, "compass", 4096, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the compass task");
        return false;
    }
    return true;
}

#else

bool compass_source_start(void)
{
    return false;
}

#endif /* NAV_COMPASS_ENABLED */
