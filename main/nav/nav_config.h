#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Runtime navigation parameters loaded from /spiffs/config/nav.json.
 * Defaults match the specification defaults.
 */
typedef struct {
    /* L1 reflex layer */
    int emergency_stop_cm;
    int l1_tick_ms;

    /* L2 FSM layer */
    int l2_tick_ms;
    int cruise_speed_pct;
    int avoid_speed_pct;
    int reverse_speed_pct;
    int avoid_trigger_cm;
    int emergency_reverse_cm;
    int clear_cm;
    int avoid_max_ms;
    int reverse_ms;
    int replan_ms;
    float arrival_radius_m;
    float heading_kp;
    float heading_ki;
    float heading_kd;
    int heading_max_steer_pct;

    /* Escalate thresholds */
    int escalate_cooldown_s;
    int stuck_window_s;
    float stuck_distance_m;
    int oscillation_window_s;
    int oscillation_count;
    float lost_distance_m;
    int goal_unreachable_window_s;

    /* GPS Kalman filter */
    float gps_sigma_pos_base_m;   /* base position noise σ (metres) */
    float gps_sigma_accel_mps2;   /* process noise — acceleration σ */
    float gps_speed_ewma_alpha;   /* EWMA alpha for speed */
    float gps_course_ewma_alpha;  /* EWMA alpha for course */
    int   gps_min_sats_accept;    /* reject obs if sats < this */
    int   gps_good_sats;          /* use R_base if sats >= this */
    int   gps_predict_timeout_ms; /* ms without accepted obs → fix=false */
    int   gps_predict_max_streak; /* predict-only count → fix=false */
    float gps_stationary_speed_mps;
    int   gps_stationary_count;   /* consecutive low-speed → clamp vx/vy */
} nav_config_t;

/**
 * Load config from SPIFFS. Falls back to spec defaults if file missing.
 */
esp_err_t nav_config_init(void);

const nav_config_t *nav_config_get(void);

#ifdef __cplusplus
}
#endif
