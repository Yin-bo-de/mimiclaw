#include "tools/tool_nav.h"
#include "nav/nav_situation.h"
#include "nav/nav_config.h"
#include "nav/nav_waypoints.h"
#include "nav/nav_planner.h"
#include "drivers/driver_gps.h"

#include "tools/tool_registry.h"
#include "esp_log.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "tool_nav";

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

esp_err_t tool_nav_save_waypoint_execute(const char *input_json, char *output, size_t output_size)
{
    char name[NAV_WAYPOINT_NAME_MAX]   = {0};
    char notes[NAV_WAYPOINT_NOTES_MAX] = {0};

    if (input_json && input_json[0] != '\0') {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *v = cJSON_GetObjectItem(root, "name");
            if (cJSON_IsString(v)) {
                strncpy(name, v->valuestring, sizeof(name) - 1);
            }
            v = cJSON_GetObjectItem(root, "notes");
            if (cJSON_IsString(v)) {
                strncpy(notes, v->valuestring, sizeof(notes) - 1);
            }
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
        snprintf(output, output_size,
                 "{\"error\":\"Save failed: %s\"}", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size,
             "{\"ok\":true,\"name\":\"%s\",\"lat\":%.6f,\"lon\":%.6f,\"sats\":%d}",
             name, gps.latitude, gps.longitude, gps.satellites);
    ESP_LOGI(TAG, "Saved waypoint '%s' (%.6f, %.6f, sats=%d)",
             name, gps.latitude, gps.longitude, gps.satellites);
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
        cJSON_AddNumberToObject(item, "lat", waypoints[i].lat);
        cJSON_AddNumberToObject(item, "lon", waypoints[i].lon);
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
            if (cJSON_IsString(v)) {
                strncpy(name, v->valuestring, sizeof(name) - 1);
            }
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
    ESP_LOGI(TAG, "Deleted waypoint '%s'", name);
    return ESP_OK;
}

esp_err_t tool_nav_status_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    nav_situation_t sit;
    nav_situation_get(&sit);

    cJSON *root = cJSON_CreateObject();
    /* Phase 4: L2 not yet running, always IDLE */
    cJSON_AddStringToObject(root, "state", "IDLE");
    cJSON_AddBoolToObject(root, "has_goal", sit.has_goal);

    if (sit.has_goal) {
        cJSON *goal = cJSON_CreateObject();
        cJSON_AddStringToObject(goal, "name", sit.goal_name);
        cJSON_AddNumberToObject(goal, "lat", sit.goal_lat);
        cJSON_AddNumberToObject(goal, "lon", sit.goal_lon);
        if (sit.gps_fix) {
            double dist = nav_planner_distance_m(sit.lat, sit.lon,
                                                  sit.goal_lat, sit.goal_lon);
            cJSON_AddNumberToObject(goal, "distance_m", dist);
        }
        cJSON_AddItemToObject(root, "goal", goal);
    }

    cJSON *gps = cJSON_CreateObject();
    cJSON_AddNumberToObject(gps, "lat", sit.lat);
    cJSON_AddNumberToObject(gps, "lon", sit.lon);
    cJSON_AddBoolToObject(gps, "fix", sit.gps_fix);
    cJSON_AddNumberToObject(gps, "sats", sit.gps_sats);
    cJSON_AddNumberToObject(gps, "speed_mps", sit.speed_mps);
    cJSON_AddItemToObject(root, "gps", gps);

    cJSON *dist = cJSON_CreateObject();
    cJSON_AddNumberToObject(dist, "left_cm", sit.distances_cm[0]);
    cJSON_AddNumberToObject(dist, "front_cm", sit.distances_cm[1]);
    cJSON_AddNumberToObject(dist, "right_cm", sit.distances_cm[2]);
    cJSON_AddItemToObject(root, "distances", dist);

    cJSON *imu = cJSON_CreateObject();
    cJSON_AddNumberToObject(imu, "yaw", sit.yaw_deg);
    cJSON_AddNumberToObject(imu, "roll", sit.roll_deg);
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
