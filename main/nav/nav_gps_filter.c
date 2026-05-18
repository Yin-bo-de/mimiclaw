#include "nav/nav_gps_filter.h"
#include "nav/nav_config.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <math.h>
#include <string.h>

static const char *TAG = "nav_gps_filter";

/* ------------------------------------------------------------------
 * Local ENU projection constants
 * ------------------------------------------------------------------ */
#define DEG_TO_M_LAT  111320.0   /* metres per degree of latitude */

/* ------------------------------------------------------------------
 * Internal Kalman state
 * ------------------------------------------------------------------ */
typedef struct {
    /* ENU origin (set on first valid fix) */
    double  origin_lat;
    double  origin_lon;
    double  m_per_deg_lon;  /* 111320 * cos(lat0) */
    bool    origin_set;

    /* State vector: x = [px, py, vx, vy]  (metres / m·s⁻¹) */
    double  x[4];
    /* Covariance matrix P (4×4, row-major) */
    double  P[16];

    /* Timestamps */
    int64_t last_obs_ts_us;
    int64_t last_accepted_ts_us;

    /* Predict-only streak */
    uint32_t predict_streak;

    /* Speed / course EWMA */
    double  speed_ewma;
    double  course_ewma;      /* unwrapped cumulative, mod 360 on output */
    bool    course_init;

    /* Stationary clamp */
    int     low_speed_count;
    bool    stationary;

    /* Satellite quality bracket tracking for INFO on crossing */
    int     last_sats_bracket; /* 0=bad, 1=weak, 2=mid, 3=good */

    /* Shared state for get_last / get_stats */
    SemaphoreHandle_t mutex;
    nav_gps_filtered_t  last_out;
    nav_gps_filter_stats_t stats;
} filter_t;

static filter_t g;

/* ------------------------------------------------------------------
 * ENU helpers
 * ------------------------------------------------------------------ */
static void latlon_to_enu(double lat, double lon, double *ex, double *ey)
{
    *ex = (lon - g.origin_lon) * g.m_per_deg_lon;
    *ey = (lat - g.origin_lat) * DEG_TO_M_LAT;
}

static void enu_to_latlon(double ex, double ey, double *lat, double *lon)
{
    *lat = g.origin_lat + ey / DEG_TO_M_LAT;
    *lon = g.origin_lon + ex / g.m_per_deg_lon;
}

static void set_origin(double lat, double lon)
{
    g.origin_lat    = lat;
    g.origin_lon    = lon;
    g.m_per_deg_lon = DEG_TO_M_LAT * cos(lat * M_PI / 180.0);
    g.origin_set    = true;
}

/* ------------------------------------------------------------------
 * 4×4 matrix helpers (row-major P[i*4+j])
 * ------------------------------------------------------------------ */
static void mat4_zero(double *M) { memset(M, 0, 16 * sizeof(double)); }

/* P = F P F^T  where F is the CV transition:
 *   F = [[1,0,dt,0],[0,1,0,dt],[0,0,1,0],[0,0,0,1]]
 * Expanded analytically to avoid a full 4×4×4 loop. */
static void kf_predict_covariance(double *P, double dt)
{
    /* Tmp = F * P  (row-by-row) */
    double T[16];
    for (int j = 0; j < 4; j++) {
        T[0*4+j] = P[0*4+j] + dt * P[2*4+j];
        T[1*4+j] = P[1*4+j] + dt * P[3*4+j];
        T[2*4+j] = P[2*4+j];
        T[3*4+j] = P[3*4+j];
    }
    /* P = Tmp * F^T  (col-by-col, but F^T transposes dt position) */
    for (int i = 0; i < 4; i++) {
        P[i*4+0] = T[i*4+0] + dt * T[i*4+2];
        P[i*4+1] = T[i*4+1] + dt * T[i*4+3];
        P[i*4+2] = T[i*4+2];
        P[i*4+3] = T[i*4+3];
    }
}

/* Add white-noise acceleration process noise Q to P */
static void kf_add_Q(double *P, double dt, double sigma_a)
{
    double q   = sigma_a * sigma_a;
    double dt2 = dt * dt;
    double dt3 = dt2 * dt;
    double dt4 = dt2 * dt2;
    P[0*4+0] += q * dt4 / 4.0;
    P[0*4+2] += q * dt3 / 2.0;
    P[2*4+0] += q * dt3 / 2.0;
    P[2*4+2] += q * dt2;
    P[1*4+1] += q * dt4 / 4.0;
    P[1*4+3] += q * dt3 / 2.0;
    P[3*4+1] += q * dt3 / 2.0;
    P[3*4+3] += q * dt2;
}

/* Kalman update with 2D position observation (H selects first 2 states) */
static void kf_update(double *x, double *P,
                      double zx, double zy,
                      double sigma_pos, double R_scale)
{
    double r = sigma_pos * sigma_pos * R_scale;

    /* Innovation: y = z - H x */
    double y0 = zx - x[0];
    double y1 = zy - x[1];

    /* S = H P H^T + R  (2×2, H picks rows 0,1) */
    double s00 = P[0*4+0] + r;
    double s01 = P[0*4+1];
    double s10 = P[1*4+0];
    double s11 = P[1*4+1] + r;

    /* S^{-1} */
    double det = s00 * s11 - s01 * s10;
    if (fabs(det) < 1e-12) return;   /* singular — skip update */
    double i00 =  s11 / det;
    double i01 = -s01 / det;
    double i10 = -s10 / det;
    double i11 =  s00 / det;

    /* K = P H^T S^{-1}  (4×2, H^T is first two columns of identity) */
    double K[8];
    for (int i = 0; i < 4; i++) {
        double p0 = P[i*4+0];
        double p1 = P[i*4+1];
        K[i*2+0] = p0 * i00 + p1 * i10;
        K[i*2+1] = p0 * i01 + p1 * i11;
    }

    /* x += K y */
    for (int i = 0; i < 4; i++) {
        x[i] += K[i*2+0] * y0 + K[i*2+1] * y1;
    }

    /* P = (I - K H) P  (H zeroes columns 2,3 of K H) */
    double Pnew[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            Pnew[i*4+j] = P[i*4+j]
                        - K[i*2+0] * P[0*4+j]
                        - K[i*2+1] * P[1*4+j];
        }
    }
    memcpy(P, Pnew, sizeof(Pnew));
}

/* ------------------------------------------------------------------
 * Satellite count → observation noise scale factor
 * ------------------------------------------------------------------ */
static double sat_to_R_scale(int sats)
{
    const nav_config_t *cfg = nav_config_get();
    if (sats >= cfg->gps_good_sats) return 1.0;
    if (sats >= 5)                  return 3.0;
    if (sats >= cfg->gps_min_sats_accept) return 12.0;
    return -1.0;  /* sentinel: reject */
}

/* ------------------------------------------------------------------
 * Satellite quality bracket (for crossing-based INFO log)
 * ------------------------------------------------------------------ */
static int sats_to_bracket(int sats, bool fix_valid)
{
    const nav_config_t *cfg = nav_config_get();
    if (!fix_valid || sats < cfg->gps_min_sats_accept) return 0;
    if (sats < 5)                return 1;
    if (sats < cfg->gps_good_sats) return 2;
    return 3;
}

/* ------------------------------------------------------------------
 * Course EWMA with unwrap
 * ------------------------------------------------------------------ */
static void course_ewma_update(double new_course, double alpha)
{
    double diff = new_course - fmod(g.course_ewma, 360.0);
    while (diff >  180.0) diff -= 360.0;
    while (diff < -180.0) diff += 360.0;
    g.course_ewma = (1.0 - alpha) * g.course_ewma + alpha * (g.course_ewma + diff);
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

esp_err_t nav_gps_filter_init(void)
{
    memset(&g, 0, sizeof(g));
    g.mutex = xSemaphoreCreateMutex();
    if (!g.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    g.last_sats_bracket = -1;
    const nav_config_t *cfg = nav_config_get();
    ESP_LOGI(TAG, "GPS filter init: sigma_pos=%.1fm sigma_a=%.1fm/s² "
             "good_sats>=%d min_sats=%d predict_max=%d",
             cfg->gps_sigma_pos_base_m, cfg->gps_sigma_accel_mps2,
             cfg->gps_good_sats, cfg->gps_min_sats_accept,
             cfg->gps_predict_max_streak);
    return ESP_OK;
}

void nav_gps_filter_reset(void)
{
    xSemaphoreTake(g.mutex, portMAX_DELAY);
    g.origin_set          = false;
    g.course_init         = false;
    g.stationary          = false;
    g.low_speed_count     = 0;
    g.predict_streak      = 0;
    g.last_accepted_ts_us = 0;
    g.last_obs_ts_us      = 0;
    memset(g.x, 0, sizeof(g.x));
    mat4_zero(g.P);
    ESP_LOGI(TAG, "Filter reset");
    xSemaphoreGive(g.mutex);
}

esp_err_t nav_gps_filter_update(const nav_gps_obs_t *obs,
                                 nav_gps_filtered_t  *out)
{
    if (!obs || !out) return ESP_ERR_INVALID_ARG;

    if (!g.mutex) {
        return ESP_ERR_INVALID_STATE;
    }

    const nav_config_t *cfg = nav_config_get();

    xSemaphoreTake(g.mutex, portMAX_DELAY);

    g.stats.total_obs++;

    /* --- dt clamp ------------------------------------------------- */
    int64_t now = obs->ts_us;
    double dt = (g.last_obs_ts_us > 0)
              ? (double)(now - g.last_obs_ts_us) / 1e6
              : 1.0;
    if (dt < 0.05) dt = 0.05;
    if (dt > 5.0)  dt = 5.0;
    g.last_obs_ts_us = now;

    /* --- accept decision ------------------------------------------ */
    double R_scale   = sat_to_R_scale(obs->sats);
    bool   can_accept = obs->fix_valid && (R_scale > 0.0);

    /* --- satellite quality bracket log (INFO on crossing) --------- */
    int bracket = sats_to_bracket(obs->sats, obs->fix_valid);
    if (bracket != g.last_sats_bracket && g.last_sats_bracket >= 0) {
        static const char *bnames[] = { "BAD(<4)", "WEAK(4)", "MID(5-6)", "GOOD(>=7)" };
        ESP_LOGI(TAG, "Sats quality: %s → %s (sats=%d)",
                 bnames[g.last_sats_bracket], bnames[bracket], obs->sats);
    }
    g.last_sats_bracket = bracket;

    /* --- first fix: initialize state ------------------------------ */
    if (!g.origin_set) {
        if (!can_accept) {
            /* No valid fix yet — report unfixed and return */
            out->lat        = obs->lat;
            out->lon        = obs->lon;
            out->speed_mps  = 0.0;
            out->course_deg = 0.0;
            out->fix        = false;
            out->sats       = obs->sats;
            out->quality    = 0;
            out->ts_us      = now;
            g.last_out = *out;
            xSemaphoreGive(g.mutex);
            return ESP_OK;
        }

        set_origin(obs->lat, obs->lon);
        g.x[0] = 0.0; g.x[1] = 0.0; g.x[2] = 0.0; g.x[3] = 0.0;

        /* Initial P: diagonal */
        double sp = cfg->gps_sigma_pos_base_m;
        mat4_zero(g.P);
        g.P[0*4+0] = sp * sp;
        g.P[1*4+1] = sp * sp;
        g.P[2*4+2] = 1.0;   /* initial speed uncertainty ~1 m/s */
        g.P[3*4+3] = 1.0;

        g.speed_ewma          = obs->speed_mps;
        g.course_ewma         = obs->course_deg;
        g.course_init         = true;
        g.last_accepted_ts_us = now;
        g.predict_streak      = 0;
        g.stats.accepted++;

        ESP_LOGI(TAG, "Origin set lat=%.6f lon=%.6f sats=%d",
                 obs->lat, obs->lon, obs->sats);

    } else {
        /* --- predict step ----------------------------------------- */
        /* x[0] += vx*dt,  x[1] += vy*dt */
        g.x[0] += g.x[2] * dt;
        g.x[1] += g.x[3] * dt;
        kf_predict_covariance(g.P, dt);
        kf_add_Q(g.P, dt, cfg->gps_sigma_accel_mps2);

        /* --- update step (if acceptable) -------------------------- */
        if (can_accept) {
            double zx, zy;
            latlon_to_enu(obs->lat, obs->lon, &zx, &zy);
            kf_update(g.x, g.P, zx, zy,
                      cfg->gps_sigma_pos_base_m, R_scale);

            /* speed EWMA */
            double a_spd = cfg->gps_speed_ewma_alpha;
            g.speed_ewma = (1.0 - a_spd) * g.speed_ewma
                         + a_spd * obs->speed_mps;

            /* course EWMA only when moving */
            if (obs->speed_mps > 0.5) {
                course_ewma_update(obs->course_deg,
                                   cfg->gps_course_ewma_alpha);
            }

            /* stationary clamp */
            if (obs->speed_mps < cfg->gps_stationary_speed_mps) {
                g.low_speed_count++;
                if (g.low_speed_count >= cfg->gps_stationary_count) {
                    if (!g.stationary) {
                        ESP_LOGI(TAG, "Enter stationary clamp (speed=%.2f)",
                                 obs->speed_mps);
                    }
                    g.stationary = true;
                    g.x[2] = 0.0;
                    g.x[3] = 0.0;
                }
            } else {
                g.low_speed_count = 0;
                if (g.stationary) {
                    ESP_LOGI(TAG, "Exit stationary clamp (speed=%.2f)",
                             obs->speed_mps);
                }
                g.stationary = false;
            }

            g.last_accepted_ts_us = now;
            g.predict_streak      = 0;

            if (R_scale > 5.0) g.stats.weakened++;
            else               g.stats.accepted++;

        } else {
            /* predict-only */
            g.predict_streak++;
            g.stats.rejected++;

            /* Log at streak milestones */
            uint32_t s = g.predict_streak;
            if (s == 1 || s == 3 || s == 5 || s == 8) {
                ESP_LOGW(TAG, "Predict-only streak=%lu fix=%d sats=%d",
                         (unsigned long)s, obs->fix_valid, obs->sats);
            }
        }
    }

    /* --- degradation check --------------------------------------- */
    int64_t ms_since_accept = (g.last_accepted_ts_us > 0)
        ? (now - g.last_accepted_ts_us) / 1000LL
        : INT64_MAX;
    bool degraded =
        (g.predict_streak >= (uint32_t)cfg->gps_predict_max_streak) ||
        (ms_since_accept  >= (int64_t)cfg->gps_predict_timeout_ms);

    if (degraded) {
        /* Stop position from drifting further */
        g.x[2] = 0.0;
        g.x[3] = 0.0;
        /* Edge-log */
        static bool was_degraded = false;
        if (!was_degraded) {
            ESP_LOGW(TAG, "GPS degraded: streak=%lu age=%lldms — fix=false",
                     (unsigned long)g.predict_streak,
                     (long long)ms_since_accept);
        }
        was_degraded = true;
    } else {
        static bool was_degraded = false;
        if (was_degraded) {
            ESP_LOGI(TAG, "GPS recovered from degraded");
        }
        was_degraded = false;
    }

    /* --- compose output ------------------------------------------ */
    double out_lat, out_lon;
    enu_to_latlon(g.x[0], g.x[1], &out_lat, &out_lon);

    double course_out = fmod(g.course_ewma, 360.0);
    if (course_out < 0.0) course_out += 360.0;

    int quality;
    if (degraded || !g.origin_set)  quality = 0;
    else if (!can_accept)           quality = 0;
    else if (R_scale > 5.0)        quality = 1;
    else if (R_scale > 1.5)        quality = 2;
    else                            quality = 3;

    out->lat        = out_lat;
    out->lon        = out_lon;
    out->speed_mps  = g.speed_ewma;
    out->course_deg = course_out;
    out->fix        = g.origin_set && !degraded;
    out->sats       = obs->sats;
    out->quality    = quality;
    out->ts_us      = now;

    g.last_out = *out;
    g.stats.predict_only_streak = g.predict_streak;
    g.stats.initialized  = g.origin_set;
    g.stats.stationary   = g.stationary;
    g.stats.origin_lat   = g.origin_lat;
    g.stats.origin_lon   = g.origin_lon;

    xSemaphoreGive(g.mutex);
    return ESP_OK;
}

void nav_gps_filter_get_last(nav_gps_filtered_t *out)
{
    if (!out) return;
    if (!g.mutex) return;
    xSemaphoreTake(g.mutex, portMAX_DELAY);
    *out = g.last_out;
    xSemaphoreGive(g.mutex);
}

void nav_gps_filter_get_stats(nav_gps_filter_stats_t *out)
{
    if (!out) return;
    if (!g.mutex) return;
    xSemaphoreTake(g.mutex, portMAX_DELAY);
    *out = g.stats;
    xSemaphoreGive(g.mutex);
}
