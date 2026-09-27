#include "gps_source.h"
#include "nav_config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "map_geo.h"

#if NAV_GPS_SOURCE == NAV_GPS_SOURCE_LC76G
#include "gps_lc76g.h"
#endif

static const char *TAG = "gps_source";

static nav_fix_cb_t       s_cb;
static void              *s_ctx;
static TaskHandle_t       s_task;
static volatile bool      s_running;
static map_route_handle_t s_route;

static void emit(const nav_fix_t *fix)
{
    if (s_cb) s_cb(fix, s_ctx);
}

/* ------------------------------------------------------------- simulator */
#if NAV_GPS_SOURCE == NAV_GPS_SOURCE_SIMULATOR

/**
 * Transport state for the demo. Written from the LVGL thread by the demo bar,
 * read by the simulator task, so every field goes through sim.lock.
 */
static struct {
    SemaphoreHandle_t lock;
    double traveled_m;      /**< Distance covered along the route */
    float  speed_scale;     /**< Multiplier on NAV_SIM_SPEED_KMH */
    bool   paused;
    bool   seek_pending;    /**< traveled_m was moved from outside the task */
    bool   straying;        /**< Demo: walk sideways off the road */
    double stray_m;         /**< Current sideways offset, metres */
} sim = {
    .speed_scale = 1.0f,
};

static inline void sim_lock(void)   { if (sim.lock) xSemaphoreTake(sim.lock, portMAX_DELAY); }
static inline void sim_unlock(void) { if (sim.lock) xSemaphoreGive(sim.lock); }

/** Uniform noise in [-amplitude, +amplitude] metres. */
static double noise_m(double amplitude)
{
    if (amplitude <= 0.0) return 0.0;
    return ((double)esp_random() / (double)UINT32_MAX - 0.5) * 2.0 * amplitude;
}

/**
 * Walk the route at a constant speed, reporting the interpolated position.
 *
 * Deliberately naive: it is here to exercise the guidance state machine and the
 * map's follow mode, not to model a vehicle. What it does model carefully is
 * being controllable, because that is what makes it useful to demo with.
 */
static void simulator_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(NAV_SIM_START_DELAY_MS));

    int point_count = map_route_get_point_count(s_route);
    if (point_count < 2) {
        ESP_LOGE(TAG, "Simulator needs a route with at least 2 points");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    const double base_mps = NAV_SIM_SPEED_KMH / 3.6;
    const double dt       = 1.0 / (double)NAV_SIM_UPDATE_HZ;
    const double total_m  = (double)map_route_get_total_distance_m(s_route);
    const bool   is_loop  = map_route_is_loop(s_route);

    int seg = 0;

    ESP_LOGI(TAG, "Simulating %s: %.2f km, %d turns, %.0f km/h at 1x%s",
             map_route_get_name(s_route), total_m / 1000.0,
             map_route_get_maneuver_count(s_route), NAV_SIM_SPEED_KMH,
             is_loop ? " (closed loop, laps forever)" : "");

    while (s_running) {
        sim_lock();
        double traveled = sim.traveled_m;
        float  scale    = sim.speed_scale;
        bool   paused   = sim.paused;
        bool   seeked   = sim.seek_pending;
        sim.seek_pending = false;
        sim_unlock();

        /* A seek can go backwards, so restart the segment walk from scratch
         * rather than assuming we only ever move forward. */
        if (seeked) seg = 0;

        while (seg < point_count - 2) {
            map_route_point_t next;
            map_route_get_point(s_route, seg + 1, &next);
            if ((double)next.cum_dist_m > traveled) break;
            seg++;
        }

        map_route_point_t a, b;
        map_route_get_point(s_route, seg, &a);
        map_route_get_point(s_route, seg + 1, &b);

        double span = (double)b.cum_dist_m - (double)a.cum_dist_m;
        double t    = (span > 0.1) ? (traveled - (double)a.cum_dist_m) / span : 0.0;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;

        double lat = a.lat + (b.lat - a.lat) * t;
        double lon = a.lon + (b.lon - a.lon) * t;

        /* The demo's stray button, on top of the usual jitter. */
        sim_lock();
        if (sim.straying) {
            sim.stray_m += NAV_DEMO_STRAY_MPS * dt;
        } else if (sim.stray_m > 0.0) {
            sim.stray_m -= NAV_DEMO_STRAY_MPS * dt;
            if (sim.stray_m < 0.0) sim.stray_m = 0.0;
        }
        double stray = sim.stray_m;
        bool   straying = sim.straying;
        sim_unlock();

        /* Offset sideways a little so the matcher has something to snap. */
        double jitter_m  = noise_m(NAV_SIM_NOISE_M) + stray;
        double m_per_lat = M_PI * MAP_GEO_EARTH_RADIUS_M / 180.0;
        double m_per_lon = m_per_lat * cos(lat * M_PI / 180.0);
        double bearing   = map_geo_bearing_deg(a.lat, a.lon, b.lat, b.lon);
        double perp_rad  = (bearing + 90.0) * M_PI / 180.0;
        lat += (jitter_m * cos(perp_rad)) / m_per_lat;
        if (fabs(m_per_lon) > 1e-6) lon += (jitter_m * sin(perp_rad)) / m_per_lon;

        bool at_end = (!is_loop && traveled >= total_m);

        double reported_heading = bearing;
        if (stray > 1.0 || straying) {
            /* Sideways movement dominates, so that is where they are pointing.
             * The direction hint is computed from heading, and would be wrong
             * if it claimed they were still facing down the road. */
            reported_heading = map_geo_normalize_deg(bearing + (straying ? 90.0 : -90.0));
        }

        nav_fix_t fix = {
            .lat          = lat,
            .lon          = lon,
            .speed_mps    = (paused || at_end) ? 0.0 : base_mps * scale,
            .heading_deg  = reported_heading,
            .valid        = true,
            .simulated    = true,
            .satellites   = NAV_SIM_SATELLITES,
            .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
        };
        emit(&fix);

        if (!paused && !at_end) {
            sim_lock();
            /* Re-read: the UI may have seeked while we were computing. */
            if (!sim.seek_pending) {
                sim.traveled_m += base_mps * scale * dt;
                if (sim.traveled_m > total_m) {
                    if (is_loop) {
                        /* Round again, keeping the overshoot so the lap does
                         * not lose a fraction of a second each time. */
                        sim.traveled_m -= total_m;
                        seg = 0;
                    } else {
                        sim.traveled_m = total_m;
                    }
                }
            }
            sim_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS((int)(dt * 1000)));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ----------------------------------------------------- demo control API */

void gps_source_sim_set_paused(bool paused)
{
    sim_lock();
    sim.paused = paused;
    sim_unlock();
    ESP_LOGI(TAG, "Demo %s", paused ? "paused" : "running");
}

bool gps_source_sim_is_paused(void)
{
    sim_lock();
    bool p = sim.paused;
    sim_unlock();
    return p;
}

void gps_source_sim_set_speed_scale(float scale)
{
    if (scale < 0.1f)  scale = 0.1f;
    if (scale > 64.0f) scale = 64.0f;
    sim_lock();
    sim.speed_scale = scale;
    sim_unlock();
    ESP_LOGI(TAG, "Demo speed %.0fx (%.0f km/h)", scale, NAV_SIM_SPEED_KMH * scale);
}

float gps_source_sim_get_speed_scale(void)
{
    sim_lock();
    float s = sim.speed_scale;
    sim_unlock();
    return s;
}

/** Move the vehicle to an absolute distance along the route. */
static void sim_seek_to(double meters)
{
    if (meters < 0.0) meters = 0.0;
    sim_lock();
    sim.traveled_m   = meters;
    sim.seek_pending = true;
    sim_unlock();
}

void gps_source_sim_restart(void)
{
    sim_seek_to(0.0);
    ESP_LOGI(TAG, "Demo restarted");
}

void gps_source_sim_skip_to_next_maneuver(void)
{
    if (!s_route) return;

    sim_lock();
    double here = sim.traveled_m;
    sim_unlock();

    int count = map_route_get_maneuver_count(s_route);
    for (int i = 0; i < count; i++) {
        map_maneuver_t m;
        if (!map_route_get_maneuver(s_route, i, &m)) continue;
        if (m.type == MAP_MANEUVER_DEPART) continue;

        /* "Next" means strictly ahead of where the lead-in would put us, so
         * pressing skip twice in a row moves on instead of sticking. */
        double target = (double)m.dist_from_start_m - NAV_DEMO_SKIP_LEAD_M;
        if (target <= here + 1.0) continue;

        sim_seek_to(target);
        ESP_LOGI(TAG, "Demo skipped to turn %d/%d (%s) at %u m",
                 i + 1, count, map_maneuver_type_name(m.type),
                 (unsigned)m.dist_from_start_m);
        return;
    }

    ESP_LOGI(TAG, "Demo: no turn left to skip to");
}

void gps_source_sim_set_straying(bool straying)
{
    sim_lock();
    sim.straying = straying;
    sim_unlock();
    ESP_LOGI(TAG, "Demo %s the route", straying ? "leaving" : "rejoining");
}

bool gps_source_sim_is_straying(void)
{
    sim_lock();
    bool v = sim.straying;
    sim_unlock();
    return v;
}

double gps_source_sim_get_traveled_m(void)
{
    sim_lock();
    double d = sim.traveled_m;
    sim_unlock();
    return d;
}

#else  /* real receiver: the demo controls compile away to nothing */

void   gps_source_sim_set_paused(bool paused)          { (void)paused; }
bool   gps_source_sim_is_paused(void)                  { return false; }
void   gps_source_sim_set_speed_scale(float scale)     { (void)scale; }
float  gps_source_sim_get_speed_scale(void)            { return 1.0f; }
void   gps_source_sim_restart(void)                    { }
void   gps_source_sim_set_straying(bool straying)      { (void)straying; }
bool   gps_source_sim_is_straying(void)                { return false; }
void   gps_source_sim_skip_to_next_maneuver(void)      { }
double gps_source_sim_get_traveled_m(void)             { return 0.0; }

#endif /* NAV_GPS_SOURCE_SIMULATOR */

/* ----------------------------------------------------------------- LC76G */

#if NAV_GPS_SOURCE == NAV_GPS_SOURCE_LC76G
static void lc76g_data_cb(const gps_data_t *data, void *user_data)
{
    (void)user_data;

    nav_fix_t fix = {
        .lat          = data->latitude,
        .lon          = data->longitude,
        .speed_mps    = data->speed_kmh / 3.6,
        .heading_deg  = data->course,
        .valid        = data->valid,
        .simulated    = false,
        .satellites   = data->satellites_used,
        .timestamp_ms = data->last_update_ms,
    };
    emit(&fix);
}

/** Report "no fix" while the receiver is still searching, so the UI can say so. */
static void lc76g_watchdog_task(void *arg)
{
    (void)arg;
    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!gps_has_fix()) {
            nav_fix_t fix = {
                .valid        = false,
                .simulated    = false,
                .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
            };
            emit(&fix);
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}
#endif

/* ------------------------------------------------------------ public API */

bool gps_source_start(map_route_handle_t route, nav_fix_cb_t cb, void *ctx)
{
    if (s_running) {
        ESP_LOGW(TAG, "Already started");
        return true;
    }

    s_cb      = cb;
    s_ctx     = ctx;
    s_route   = route;
    s_running = true;

#if NAV_GPS_SOURCE == NAV_GPS_SOURCE_SIMULATOR
    if (!route) {
        ESP_LOGE(TAG, "The simulator needs a route to drive along");
        s_running = false;
        return false;
    }
    if (!sim.lock) {
        sim.lock = xSemaphoreCreateMutex();
        if (!sim.lock) {
            ESP_LOGE(TAG, "Cannot create the simulator mutex");
            s_running = false;
            return false;
        }
    }
    sim.traveled_m   = 0.0;
    sim.paused       = false;
    sim.seek_pending = false;
    sim.straying     = false;
    sim.stray_m      = 0.0;

    if (xTaskCreate(simulator_task, "gps_sim", 4096, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the simulator task");
        s_running = false;
        return false;
    }
    return true;
#else
    gps_config_t cfg = {
        .uart_num       = NAV_GPS_UART_NUM,
        .tx_pin         = NAV_GPS_UART_TX_PIN,
        .rx_pin         = NAV_GPS_UART_RX_PIN,
        .baud_rate      = NAV_GPS_UART_BAUD,
        .update_rate_hz = NAV_GPS_UPDATE_HZ,
    };

    esp_err_t err = gps_init_with_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gps_init_with_config failed: %s", esp_err_to_name(err));
        s_running = false;
        return false;
    }

    /* gps_init_with_config() installs the UART but does not push the rate to
     * the module, so ask for it explicitly. At the module's 1 Hz default a
     * vehicle moves 14 m between fixes at 50 km/h, which the map shows as the
     * marker hopping rather than moving. */
    esp_err_t rate_err = gps_set_update_rate(NAV_GPS_UPDATE_HZ);
    if (rate_err != ESP_OK) {
        ESP_LOGW(TAG, "Module kept its default rate (%s); the marker will step "
                      "rather than glide", esp_err_to_name(rate_err));
    } else {
        ESP_LOGI(TAG, "GPS update rate set to %d Hz", NAV_GPS_UPDATE_HZ);
    }

    gps_register_data_callback(lc76g_data_cb, NULL);

    if (xTaskCreate(lc76g_watchdog_task, "gps_watch", 3072, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start the GPS watchdog task");
        s_running = false;
        return false;
    }
    return true;
#endif
}

void gps_source_stop(void)
{
    if (!s_running) return;
    s_running = false;

    for (int i = 0; i < 50 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

#if NAV_GPS_SOURCE == NAV_GPS_SOURCE_LC76G
    gps_deinit();
#endif

    s_cb  = NULL;
    s_ctx = NULL;
}

const char *gps_source_name(void)
{
#if NAV_GPS_SOURCE == NAV_GPS_SOURCE_SIMULATOR
    return "Simulator";
#else
    return "LC76G";
#endif
}

bool gps_source_is_simulated(void)
{
#if NAV_GPS_SOURCE == NAV_GPS_SOURCE_SIMULATOR
    return true;
#else
    return false;
#endif
}
