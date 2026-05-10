#include "nav/nav_waypoints.h"
#include "mimi_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nav_waypoints";

static SemaphoreHandle_t s_mutex;
static nav_waypoint_t s_waypoints[NAV_WAYPOINTS_MAX];
static int s_count = 0;

static esp_err_t load_file(void)
{
    FILE *f = fopen(MIMI_NAV_WAYPOINTS_FILE, "r");
    if (!f) {
        s_count = 0;
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
        s_count = 0;
        return ESP_OK;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "waypoints");
    s_count = 0;
    if (cJSON_IsArray(arr)) {
        cJSON *item;
        cJSON_ArrayForEach(item, arr) {
            if (s_count >= NAV_WAYPOINTS_MAX) break;
            nav_waypoint_t *wp = &s_waypoints[s_count];
            memset(wp, 0, sizeof(*wp));

            cJSON *v = cJSON_GetObjectItem(item, "name");
            if (cJSON_IsString(v)) {
                strncpy(wp->name, v->valuestring, NAV_WAYPOINT_NAME_MAX - 1);
            }
            v = cJSON_GetObjectItem(item, "lat");
            if (cJSON_IsNumber(v)) wp->lat = v->valuedouble;
            v = cJSON_GetObjectItem(item, "lon");
            if (cJSON_IsNumber(v)) wp->lon = v->valuedouble;
            v = cJSON_GetObjectItem(item, "saved_at");
            if (cJSON_IsNumber(v)) wp->saved_at = (uint32_t)v->valuedouble;

            cJSON *fq = cJSON_GetObjectItem(item, "fix_quality");
            if (fq) {
                v = cJSON_GetObjectItem(fq, "sats");
                if (cJSON_IsNumber(v)) wp->fix_sats = (int)v->valuedouble;
                v = cJSON_GetObjectItem(fq, "hdop");
                if (cJSON_IsNumber(v)) wp->hdop = (float)v->valuedouble;
            }

            v = cJSON_GetObjectItem(item, "notes");
            if (cJSON_IsString(v)) {
                strncpy(wp->notes, v->valuestring, NAV_WAYPOINT_NOTES_MAX - 1);
            }

            s_count++;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t save_file(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_count; i++) {
        nav_waypoint_t *wp = &s_waypoints[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", wp->name);
        cJSON_AddNumberToObject(item, "lat", wp->lat);
        cJSON_AddNumberToObject(item, "lon", wp->lon);
        cJSON_AddNumberToObject(item, "saved_at", (double)wp->saved_at);

        cJSON *fq = cJSON_CreateObject();
        cJSON_AddNumberToObject(fq, "sats", wp->fix_sats);
        cJSON_AddNumberToObject(fq, "hdop", (double)wp->hdop);
        cJSON_AddItemToObject(item, "fix_quality", fq);

        cJSON_AddStringToObject(item, "notes", wp->notes);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "waypoints", arr);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;

    FILE *f = fopen(MIMI_NAV_WAYPOINTS_FILE, "w");
    if (!f) {
        free(json_str);
        ESP_LOGE(TAG, "Cannot open %s for write", MIMI_NAV_WAYPOINTS_FILE);
        return ESP_FAIL;
    }
    fputs(json_str, f);
    fclose(f);
    free(json_str);
    return ESP_OK;
}

esp_err_t nav_waypoints_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = load_file();
    ESP_LOGI(TAG, "nav_waypoints initialized: %d waypoints loaded", s_count);
    return err;
}

esp_err_t nav_waypoints_save(const char *name, double lat, double lon,
                              int sats, float hdop, const char *notes)
{
    if (!name || name[0] == '\0') return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_waypoints[i].name, name) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (s_count >= NAV_WAYPOINTS_MAX) {
            xSemaphoreGive(s_mutex);
            ESP_LOGE(TAG, "Waypoint storage full (%d)", NAV_WAYPOINTS_MAX);
            return ESP_ERR_NO_MEM;
        }
        idx = s_count++;
    }

    nav_waypoint_t *wp = &s_waypoints[idx];
    memset(wp, 0, sizeof(*wp));
    strncpy(wp->name, name, NAV_WAYPOINT_NAME_MAX - 1);
    wp->lat      = lat;
    wp->lon      = lon;
    wp->saved_at = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    wp->fix_sats = sats;
    wp->hdop     = hdop;
    if (notes) strncpy(wp->notes, notes, NAV_WAYPOINT_NOTES_MAX - 1);

    esp_err_t err = save_file();
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t nav_waypoints_find(const char *name, nav_waypoint_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_waypoints[i].name, name) == 0) {
            *out = s_waypoints[i];
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t nav_waypoints_delete(const char *name)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_waypoints[i].name, name) == 0) {
            for (int j = i; j < s_count - 1; j++) {
                s_waypoints[j] = s_waypoints[j + 1];
            }
            s_count--;
            esp_err_t err = save_file();
            xSemaphoreGive(s_mutex);
            return err;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

int nav_waypoints_list(nav_waypoint_t *buf, int max_count)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = (s_count < max_count) ? s_count : max_count;
    for (int i = 0; i < n; i++) buf[i] = s_waypoints[i];
    xSemaphoreGive(s_mutex);
    return n;
}
