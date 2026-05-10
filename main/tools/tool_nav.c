#include "tools/tool_nav.h"
#include "nav/nav_situation.h"
#include "nav/nav_config.h"
#include "nav/nav_waypoints.h"
#include "nav/nav_planner.h"
#include "nav/nav_controller.h"
#include "nav/nav_l2_fsm.h"
#include "drivers/driver_gps.h"
#include "tools/tool_pwm.h"
#include "tools/tool_registry.h"

#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "tool_nav";

/* ------------------------------------------------------------------ */
/*  Init                                                                */
/* ------------------------------------------------------------------ */

esp_err_t tool_nav_init(void)
{
    esp_err_t err;

    err = nav_situation_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nav_situation_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nav_config_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nav_config_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nav_waypoints_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nav_waypoints_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "tool_nav initialized");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Phase 4: Waypoint management                                        */
/* ------------------------------------------------------------------ */

esp_err_t tool_nav_save_waypoint_execute(const char *input_json, char *output, size_t output_size)
{
    char name[NAV_WAYPOINT_NAME_MAX]   = {0};
    char notes[NAV_WAYPOINT_NOTES_MAX] = {0};

    if (input_json && input_json[0] != '\0') {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *v = cJSON_GetObjectItem(root, "name");
            if (cJSON_IsString(v)) strncpy(name, v->valuestring, sizeof(name) - 1);
            v = cJSON_GetObjectItem(root, "notes");
            if (cJSON_IsString(v)) strncpy(notes, v->valuestring, sizeof(notes) - 1);
            cJSON_Delete(root);
        }
    }

    if (name[0] == '\0') {
        snprintf(output, output_size, "{\"error\":\"'name' is required\"}");
        return ESP_ERR_INVALID_ARG;
    }

    gps_reading_t gps = driver_gps_get_reading();
    if (!gps.fix_valid) {
        snprintf(output, output_size,
                 "{\"error\":\"No GPS fix. Cannot save waypoint.\",\"sats\":%d}",
                 gps.satellites);
        return ESP_FAIL;
    }

    esp_err_t err = nav_waypoints_save(name, gps.latitude, gps.longitude,
                                        gps.satellites, 0.0f, notes);
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"error\":\"Save failed: %s\"}", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size,
             "{\"ok\":true,\"name\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,\"sats\":%d}",
             name, gps.latitude, gps.longitude, gps.satellites);
    ESP_LOGI(TAG, "Saved waypoint '%s' (%.6f, %.6f)", name, gps.latitude, gps.longitude);
    return ESP_OK;
}

esp_err_t tool_nav_list_waypoints_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    nav_waypoint_t waypoints[NAV_WAYPOINTS_MAX];
    int count = nav_waypoints_list(waypoints, NAV_WAYPOINTS_MAX);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", count);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", waypoints[i].name);
        cJSON_AddNumberToObject(item, "lat",  waypoints[i].lat);
        cJSON_AddNumberToObject(item, "lon",  waypoints[i].lon);
        cJSON_AddNumberToObject(item, "sats", waypoints[i].fix_sats);
        if (waypoints[i].notes[0] != '\0') {
            cJSON_AddStringToObject(item, "notes", waypoints[i].notes);
        }
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "waypoints", arr);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str) {
        strncpy(output, json_str, output_size - 1);
        output[output_size - 1] = '\0';
        free(json_str);
    }
    return ESP_OK;
}

esp_err_t tool_nav_delete_waypoint_execute(const char *input_json, char *output, size_t output_size)
{
    char name[NAV_WAYPOINT_NAME_MAX] = {0};

    if (input_json && input_json[0] != '\0') {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *v = cJSON_GetObjectItem(root, "name");
            if (cJSON_IsString(v)) strncpy(name, v->valuestring, sizeof(name) - 1);
            cJSON_Delete(root);
        }
    }

    if (name[0] == '\0') {
        snprintf(output, output_size, "{\"error\":\"'name' is required\"}");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = nav_waypoints_delete(name);
    if (err == ESP_ERR_NOT_FOUND) {
        snprintf(output, output_size, "{\"error\":\"Waypoint '%s' not found\"}", name);
        return err;
    }
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"error\":\"Delete failed: %s\"}", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "{\"ok\":true,\"deleted\":\"%s\"}", name);
    return ESP_OK;
}

esp_err_t tool_nav_status_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    nav_situation_t sit;
    nav_situation_get(&sit);

    cJSON *root = cJSON_CreateObject();

    /* Controller and L2 FSM state */
    cJSON_AddStringToObject(root, "state",    nav_ctrl_state_name(nav_controller_get_state()));
    cJSON_AddStringToObject(root, "l2_state", nav_controller_get_l2_state_name());
    cJSON_AddBoolToObject(root,   "has_goal", sit.has_goal);
    cJSON_AddNumberToObject(root, "avoid_count",  nav_l2_get_avoid_count());
    cJSON_AddNumberToObject(root, "replan_count", nav_l2_get_replan_count());

    /* Active goal */
    if (sit.has_goal) {
        cJSON *goal = cJSON_CreateObject();
        cJSON_AddStringToObject(goal, "name", sit.goal_name);
        cJSON_AddNumberToObject(goal, "lat",  sit.goal_lat);
        cJSON_AddNumberToObject(goal, "lon",  sit.goal_lon);
        if (sit.gps_fix) {
            double dist = nav_planner_distance_m(sit.lat, sit.lon,
                                                  sit.goal_lat, sit.goal_lon);
            double bearing = nav_planner_bearing_deg(sit.lat, sit.lon,
                                                      sit.goal_lat, sit.goal_lon);
            cJSON_AddNumberToObject(goal, "distance_m",  dist);
            cJSON_AddNumberToObject(goal, "bearing_deg", bearing);
        }
        cJSON_AddItemToObject(root, "goal", goal);
    }

    /* GPS */
    cJSON *gps = cJSON_CreateObject();
    cJSON_AddNumberToObject(gps, "lat",       sit.lat);
    cJSON_AddNumberToObject(gps, "lon",       sit.lon);
    cJSON_AddBoolToObject(gps,   "fix",       sit.gps_fix);
    cJSON_AddNumberToObject(gps, "sats",      sit.gps_sats);
    cJSON_AddNumberToObject(gps, "speed_mps", sit.speed_mps);
    cJSON_AddNumberToObject(gps, "course_deg",sit.course_deg);
    cJSON_AddItemToObject(root, "gps", gps);

    /* Distances */
    cJSON *dist = cJSON_CreateObject();
    cJSON_AddNumberToObject(dist, "left_cm",  sit.distances_cm[0]);
    cJSON_AddNumberToObject(dist, "front_cm", sit.distances_cm[1]);
    cJSON_AddNumberToObject(dist, "right_cm", sit.distances_cm[2]);
    cJSON_AddItemToObject(root, "distances", dist);

    /* IMU */
    cJSON *imu = cJSON_CreateObject();
    cJSON_AddNumberToObject(imu, "yaw",   sit.yaw_deg);
    cJSON_AddNumberToObject(imu, "roll",  sit.roll_deg);
    cJSON_AddNumberToObject(imu, "pitch", sit.pitch_deg);
    cJSON_AddItemToObject(root, "imu", imu);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str) {
        strncpy(output, json_str, output_size - 1);
        output[output_size - 1] = '\0';
        free(json_str);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Phase 6: Autonomous navigation control                              */
/* ------------------------------------------------------------------ */

esp_err_t tool_nav_goto_execute(const char *input_json, char *output, size_t output_size)
{
    double lat = 0.0, lon = 0.0;
    int speed_pct = 35;
    bool has_lat = false, has_lon = false;

    if (input_json && input_json[0] != '\0') {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *v;
            v = cJSON_GetObjectItem(root, "lat");
            if (cJSON_IsNumber(v)) { lat = v->valuedouble; has_lat = true; }
            v = cJSON_GetObjectItem(root, "lon");
            if (cJSON_IsNumber(v)) { lon = v->valuedouble; has_lon = true; }
            v = cJSON_GetObjectItem(root, "speed_pct");
            if (cJSON_IsNumber(v)) speed_pct = (int)v->valuedouble;
            cJSON_Delete(root);
        }
    }

    if (!has_lat || !has_lon) {
        snprintf(output, output_size, "{\"error\":\"'lat' and 'lon' are required\"}");
        return ESP_ERR_INVALID_ARG;
    }
    if (speed_pct < 1 || speed_pct > 100) speed_pct = 35;

    esp_err_t err = nav_controller_set_goal(lat, lon, "custom", speed_pct);
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"error\":\"Failed to set goal: %s\"}",
                 esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size,
             "{\"ok\":true,\"lat\":%.6f,\"lon\":%.6f,\"speed_pct\":%d}",
             lat, lon, speed_pct);
    return ESP_OK;
}

esp_err_t tool_nav_goto_waypoint_execute(const char *input_json, char *output, size_t output_size)
{
    char name[NAV_WAYPOINT_NAME_MAX] = {0};
    int speed_pct = 35;

    if (input_json && input_json[0] != '\0') {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *v;
            v = cJSON_GetObjectItem(root, "name");
            if (cJSON_IsString(v)) strncpy(name, v->valuestring, sizeof(name) - 1);
            v = cJSON_GetObjectItem(root, "speed_pct");
            if (cJSON_IsNumber(v)) speed_pct = (int)v->valuedouble;
            cJSON_Delete(root);
        }
    }

    if (name[0] == '\0') {
        snprintf(output, output_size, "{\"error\":\"'name' is required\"}");
        return ESP_ERR_INVALID_ARG;
    }
    if (speed_pct < 1 || speed_pct > 100) speed_pct = 35;

    esp_err_t err = nav_controller_set_goal_by_waypoint(name, speed_pct);
    if (err == ESP_ERR_NOT_FOUND) {
        snprintf(output, output_size, "{\"error\":\"Waypoint '%s' not found\"}", name);
        return err;
    }
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"error\":\"Navigation failed: %s\"}",
                 esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size,
             "{\"ok\":true,\"waypoint\":\"%s\",\"speed_pct\":%d}", name, speed_pct);
    ESP_LOGI(TAG, "nav_goto_waypoint: '%s' at %d%%", name, speed_pct);
    return ESP_OK;
}

esp_err_t tool_nav_pause_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    if (!nav_l2_is_running()) {
        snprintf(output, output_size, "{\"error\":\"Navigation is not active\"}");
        return ESP_ERR_INVALID_STATE;
    }
    nav_controller_pause();
    snprintf(output, output_size, "{\"ok\":true,\"state\":\"PAUSED\"}");
    return ESP_OK;
}

esp_err_t tool_nav_resume_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    if (!nav_l2_is_running()) {
        snprintf(output, output_size, "{\"error\":\"Navigation is not active\"}");
        return ESP_ERR_INVALID_STATE;
    }
    nav_controller_resume();
    snprintf(output, output_size, "{\"ok\":true,\"state\":\"RUNNING\"}");
    return ESP_OK;
}

esp_err_t tool_nav_abort_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    nav_controller_abort();
    snprintf(output, output_size, "{\"ok\":true,\"state\":\"IDLE\"}");
    return ESP_OK;
}

esp_err_t tool_nav_manual_step_execute(const char *input_json, char *output, size_t output_size)
{
    int steer_pct    = 0;
    int throttle_pct = 0;
    int hold_ms      = 200;

    if (input_json && input_json[0] != '\0') {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *v;
            v = cJSON_GetObjectItem(root, "steer_pct");
            if (cJSON_IsNumber(v)) steer_pct = (int)v->valuedouble;
            v = cJSON_GetObjectItem(root, "throttle_pct");
            if (cJSON_IsNumber(v)) throttle_pct = (int)v->valuedouble;
            v = cJSON_GetObjectItem(root, "hold_ms");
            if (cJSON_IsNumber(v)) hold_ms = (int)v->valuedouble;
            cJSON_Delete(root);
        }
    }

    /* Clamp hold_ms to 1 second per spec */
    if (hold_ms < 0)    hold_ms = 0;
    if (hold_ms > 1000) hold_ms = 1000;

    /* Pause L2 during manual step */
    bool was_running = nav_l2_is_running();
    if (was_running) nav_controller_pause();

    /* Apply override — L1 (prio=7) still guards emergency stop */
    rc_nav_steer(steer_pct);
    rc_nav_throttle(throttle_pct);

    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    /* Stop and resume L2 */
    rc_nav_throttle(0);
    rc_nav_steer(0);
    if (was_running) nav_controller_resume();

    snprintf(output, output_size,
             "{\"ok\":true,\"steer_pct\":%d,\"throttle_pct\":%d,\"hold_ms\":%d}",
             steer_pct, throttle_pct, hold_ms);
    return ESP_OK;
}
