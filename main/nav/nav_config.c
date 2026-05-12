#include "nav/nav_config.h"
#include "mimi_config.h"

#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nav_config";

/* Defaults mirror /spiffs/config/nav.json spec values */
static nav_config_t s_config = {
    .emergency_stop_cm        = 20,
    .l1_tick_ms               = 20,
    .l2_tick_ms               = 100,
    .cruise_speed_pct         = 35,
    .avoid_speed_pct          = 20,
    .reverse_speed_pct        = 25,
    .avoid_trigger_cm         = 60,
    .emergency_reverse_cm     = 25,
    .clear_cm                 = 100,
    .avoid_max_ms             = 2000,
    .reverse_ms               = 800,
    .replan_ms                = 1500,
    .arrival_radius_m         = 3.0f,
    .heading_kp               = 2.0f,
    .heading_max_steer_pct    = 100,
    .escalate_cooldown_s      = 60,
    .stuck_window_s           = 10,
    .stuck_distance_m         = 0.30f,
    .oscillation_window_s     = 15,
    .oscillation_count        = 4,
    .lost_distance_m          = 20.0f,
    .goal_unreachable_window_s = 30,

    /* GPS Kalman filter defaults */
    .gps_sigma_pos_base_m    = 2.0f,
    .gps_sigma_accel_mps2    = 1.0f,
    .gps_speed_ewma_alpha    = 0.4f,
    .gps_course_ewma_alpha   = 0.3f,
    .gps_min_sats_accept     = 4,
    .gps_good_sats           = 7,
    .gps_predict_timeout_ms  = 5000,
    .gps_predict_max_streak  = 8,
    .gps_stationary_speed_mps = 0.3f,
    .gps_stationary_count    = 3,
};

static void parse_json(cJSON *root)
{
    cJSON *v;

    cJSON *l1 = cJSON_GetObjectItem(root, "l1");
    if (l1) {
        v = cJSON_GetObjectItem(l1, "emergency_stop_cm");
        if (cJSON_IsNumber(v)) s_config.emergency_stop_cm = (int)v->valuedouble;
        v = cJSON_GetObjectItem(l1, "tick_period_ms");
        if (cJSON_IsNumber(v)) s_config.l1_tick_ms = (int)v->valuedouble;
    }

    cJSON *l2 = cJSON_GetObjectItem(root, "l2");
    if (l2) {
        v = cJSON_GetObjectItem(l2, "tick_period_ms");
        if (cJSON_IsNumber(v)) s_config.l2_tick_ms = (int)v->valuedouble;
        v = cJSON_GetObjectItem(l2, "cruise_speed_pct");
        if (cJSON_IsNumber(v)) s_config.cruise_speed_pct = (int)v->valuedouble;
        v = cJSON_GetObjectItem(l2, "avoid_speed_pct");
        if (cJSON_IsNumber(v)) s_config.avoid_speed_pct = (int)v->valuedouble;
        v = cJSON_GetObjectItem(l2, "reverse_speed_pct");
        if (cJSON_IsNumber(v)) s_config.reverse_speed_pct = (int)v->valuedouble;
        v = cJSON_GetObjectItem(l2, "avoid_trigger_cm");
        if (cJSON_IsNumber(v)) s_config.avoid_trigger_cm = (int)v->valuedouble;
        v = cJSON_GetObjectItem(l2, "emergency_reverse_cm");
        if (cJSON_IsNumber(v)) s_config.emergency_reverse_cm = (int)v->valuedouble;
        v = cJSON_GetObjectItem(l2, "clear_cm");
        if (cJSON_IsNumber(v)) s_config.clear_cm = (int)v->valuedouble;
        v = cJSON_GetObjectItem(l2, "avoid_max_ms");
        if (cJSON_IsNumber(v)) s_config.avoid_max_ms = (int)v->valuedouble;
        v = cJSON_GetObjectItem(l2, "reverse_ms");
        if (cJSON_IsNumber(v)) s_config.reverse_ms = (int)v->valuedouble;
        v = cJSON_GetObjectItem(l2, "replan_ms");
        if (cJSON_IsNumber(v)) s_config.replan_ms = (int)v->valuedouble;
        v = cJSON_GetObjectItem(l2, "arrival_radius_m");
        if (cJSON_IsNumber(v)) s_config.arrival_radius_m = (float)v->valuedouble;
        v = cJSON_GetObjectItem(l2, "heading_kp");
        if (cJSON_IsNumber(v)) s_config.heading_kp = (float)v->valuedouble;
        v = cJSON_GetObjectItem(l2, "heading_max_steer_pct");
        if (cJSON_IsNumber(v)) s_config.heading_max_steer_pct = (int)v->valuedouble;
    }

    cJSON *gpsf = cJSON_GetObjectItem(root, "gps_filter");
    if (gpsf) {
        v = cJSON_GetObjectItem(gpsf, "sigma_pos_base_m");
        if (cJSON_IsNumber(v)) s_config.gps_sigma_pos_base_m = (float)v->valuedouble;
        v = cJSON_GetObjectItem(gpsf, "sigma_accel_mps2");
        if (cJSON_IsNumber(v)) s_config.gps_sigma_accel_mps2 = (float)v->valuedouble;
        v = cJSON_GetObjectItem(gpsf, "speed_ewma_alpha");
        if (cJSON_IsNumber(v)) s_config.gps_speed_ewma_alpha = (float)v->valuedouble;
        v = cJSON_GetObjectItem(gpsf, "course_ewma_alpha");
        if (cJSON_IsNumber(v)) s_config.gps_course_ewma_alpha = (float)v->valuedouble;
        v = cJSON_GetObjectItem(gpsf, "min_sats_accept");
        if (cJSON_IsNumber(v)) s_config.gps_min_sats_accept = (int)v->valuedouble;
        v = cJSON_GetObjectItem(gpsf, "good_sats");
        if (cJSON_IsNumber(v)) s_config.gps_good_sats = (int)v->valuedouble;
        v = cJSON_GetObjectItem(gpsf, "predict_timeout_ms");
        if (cJSON_IsNumber(v)) s_config.gps_predict_timeout_ms = (int)v->valuedouble;
        v = cJSON_GetObjectItem(gpsf, "predict_max_streak");
        if (cJSON_IsNumber(v)) s_config.gps_predict_max_streak = (int)v->valuedouble;
        v = cJSON_GetObjectItem(gpsf, "stationary_speed_mps");
        if (cJSON_IsNumber(v)) s_config.gps_stationary_speed_mps = (float)v->valuedouble;
        v = cJSON_GetObjectItem(gpsf, "stationary_count");
        if (cJSON_IsNumber(v)) s_config.gps_stationary_count = (int)v->valuedouble;
    }

    cJSON *esc = cJSON_GetObjectItem(root, "escalate");
    if (esc) {
        v = cJSON_GetObjectItem(esc, "cooldown_s");
        if (cJSON_IsNumber(v)) s_config.escalate_cooldown_s = (int)v->valuedouble;
        v = cJSON_GetObjectItem(esc, "stuck_window_s");
        if (cJSON_IsNumber(v)) s_config.stuck_window_s = (int)v->valuedouble;
        v = cJSON_GetObjectItem(esc, "stuck_distance_m");
        if (cJSON_IsNumber(v)) s_config.stuck_distance_m = (float)v->valuedouble;
        v = cJSON_GetObjectItem(esc, "oscillation_window_s");
        if (cJSON_IsNumber(v)) s_config.oscillation_window_s = (int)v->valuedouble;
        v = cJSON_GetObjectItem(esc, "oscillation_count");
        if (cJSON_IsNumber(v)) s_config.oscillation_count = (int)v->valuedouble;
        v = cJSON_GetObjectItem(esc, "lost_distance_m");
        if (cJSON_IsNumber(v)) s_config.lost_distance_m = (float)v->valuedouble;
        v = cJSON_GetObjectItem(esc, "goal_unreachable_window_s");
        if (cJSON_IsNumber(v)) s_config.goal_unreachable_window_s = (int)v->valuedouble;
    }
}

esp_err_t nav_config_init(void)
{
    FILE *f = fopen(MIMI_NAV_PARAMS_FILE, "r");
    if (!f) {
        ESP_LOGW(TAG, "nav.json not found, using spec defaults");
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "nav.json parse failed, using spec defaults");
        return ESP_OK;
    }

    parse_json(root);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "nav_config loaded from %s", MIMI_NAV_PARAMS_FILE);
    return ESP_OK;
}

const nav_config_t *nav_config_get(void)
{
    return &s_config;
}
