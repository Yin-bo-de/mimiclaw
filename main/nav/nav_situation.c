#include "nav/nav_situation.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "nav_situation";

static nav_situation_t s_situation;
static SemaphoreHandle_t s_mutex;

esp_err_t nav_situation_init(void)
{
    memset(&s_situation, 0, sizeof(s_situation));
    /* Default distances to max range so L1 does not trigger on startup */
    for (int i = 0; i < 3; i++) {
        s_situation.distances_cm[i] = 400;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "nav_situation initialized");
    return ESP_OK;
}

void nav_situation_get(nav_situation_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_situation;
    xSemaphoreGive(s_mutex);
}

void nav_situation_update_distances(int left_cm, int front_cm, int right_cm,
                                    bool left_valid, bool front_valid, bool right_valid)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_situation.distances_cm[0] = left_cm;
    s_situation.distances_cm[1] = front_cm;
    s_situation.distances_cm[2] = right_cm;
    s_situation.distance_valid[0] = left_valid;
    s_situation.distance_valid[1] = front_valid;
    s_situation.distance_valid[2] = right_valid;
    s_situation.distance_ts_us[0] = now;
    s_situation.distance_ts_us[1] = now;
    s_situation.distance_ts_us[2] = now;
    xSemaphoreGive(s_mutex);
}

void nav_situation_update_imu(float roll, float pitch, float yaw, float gz)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_situation.roll_deg  = roll;
    s_situation.pitch_deg = pitch;
    s_situation.yaw_deg   = yaw;
    s_situation.gz_dps    = gz;
    s_situation.imu_valid  = true;
    s_situation.imu_ts_us  = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
}

void nav_situation_update_gps(double lat, double lon, bool fix, int sats,
                               double speed_mps, double course_deg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_situation.lat        = lat;
    s_situation.lon        = lon;
    s_situation.gps_fix    = fix;
    s_situation.gps_sats   = sats;
    s_situation.speed_mps  = speed_mps;
    s_situation.course_deg = course_deg;
    s_situation.gps_ts_us  = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
}

void nav_situation_set_goal(double lat, double lon, const char *name)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_situation.goal_lat = lat;
    s_situation.goal_lon = lon;
    strncpy(s_situation.goal_name, name ? name : "", sizeof(s_situation.goal_name) - 1);
    s_situation.goal_name[sizeof(s_situation.goal_name) - 1] = '\0';
    s_situation.has_goal = true;
    xSemaphoreGive(s_mutex);
}

void nav_situation_clear_goal(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_situation.has_goal       = false;
    s_situation.goal_name[0]   = '\0';
    xSemaphoreGive(s_mutex);
}
