#include "nav/nav_controller.h"
#include "nav/nav_l1_reflex.h"
#include "nav/nav_l2_fsm.h"
#include "nav/nav_situation.h"
#include "nav/nav_waypoints.h"
#include "tools/tool_pwm.h"
#include "mimi_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "nav_ctrl";

static nav_ctrl_state_t s_state = NAV_CTRL_IDLE;
static volatile bool s_dummy_running = false;
static int s_dummy_speed_pct = 0;

const char *nav_ctrl_state_name(nav_ctrl_state_t s)
{
    switch (s) {
    case NAV_CTRL_IDLE:    return "IDLE";
    case NAV_CTRL_RUNNING: return "RUNNING";
    case NAV_CTRL_PAUSED:  return "PAUSED";
    default:               return "UNKNOWN";
    }
}

nav_ctrl_state_t nav_controller_get_state(void) { return s_state; }
bool nav_controller_dummy_is_running(void) { return s_dummy_running; }

/* ------------------------------------------------------------------ */
/*  Init / Start                                                        */
/* ------------------------------------------------------------------ */

esp_err_t nav_controller_init(void)
{
    esp_err_t err = nav_l1_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nav_l1_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nav_l2_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nav_l2_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "nav_controller initialized");
    return ESP_OK;
}

esp_err_t nav_controller_start(void)
{
    esp_err_t err = nav_l1_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nav_l1_start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "nav_controller started — L1 reflex active");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Goal management                                                     */
/* ------------------------------------------------------------------ */

esp_err_t nav_controller_set_goal(double lat, double lon,
                                   const char *name, int speed_pct)
{
    if (speed_pct < 1 || speed_pct > 100) speed_pct = 35; /* spec default */

    /* Stop dummy drive if active */
    if (s_dummy_running) {
        nav_controller_dummy_drive_stop();
    }

    /* Update situation goal */
    nav_situation_set_goal(lat, lon, name);

    esp_err_t err = nav_l2_start(lat, lon, name, speed_pct);
    if (err != ESP_OK) return err;

    s_state = NAV_CTRL_RUNNING;
    ESP_LOGI(TAG, "Goal set: '%s' (%.6f, %.6f) speed=%d%%", name, lat, lon, speed_pct);
    return ESP_OK;
}

esp_err_t nav_controller_set_goal_by_waypoint(const char *name, int speed_pct)
{
    nav_waypoint_t wp;
    esp_err_t err = nav_waypoints_find(name, &wp);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Waypoint '%s' not found", name);
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) return err;
    return nav_controller_set_goal(wp.lat, wp.lon, name, speed_pct);
}

void nav_controller_pause(void)
{
    if (nav_l2_is_running()) {
        nav_l2_pause();
        s_state = NAV_CTRL_PAUSED;
        ESP_LOGI(TAG, "Navigation paused");
    }
}

void nav_controller_resume(void)
{
    if (nav_l2_is_running()) {
        nav_l2_resume();
        s_state = NAV_CTRL_RUNNING;
        ESP_LOGI(TAG, "Navigation resumed");
    }
}

void nav_controller_abort(void)
{
    if (nav_l2_is_running()) {
        nav_l2_abort();
        /* Phase 7: escalate ESC_ABORTED */
    }
    nav_situation_clear_goal();
    s_state = NAV_CTRL_IDLE;
    ESP_LOGI(TAG, "Navigation aborted");
}

const char *nav_controller_get_l2_state_name(void)
{
    if (!nav_l2_is_running()) return "IDLE";
    return nav_l2_get_state_name();
}

/* ------------------------------------------------------------------ */
/*  Phase 5 dummy drive (kept for backward compatibility)               */
/* ------------------------------------------------------------------ */

static void dummy_drive_task(void *arg)
{
    ESP_LOGI(TAG, "Dummy drive task started at %d%%", s_dummy_speed_pct);
    rc_nav_steer(0);

    while (s_dummy_running) {
        if (!nav_l1_is_blocked()) {
            rc_nav_throttle(s_dummy_speed_pct);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    rc_nav_throttle(0);
    rc_nav_steer(0);
    ESP_LOGI(TAG, "Dummy drive task exiting");
    vTaskDelete(NULL);
}

esp_err_t nav_controller_dummy_drive_start(int speed_pct)
{
    if (speed_pct < 1 || speed_pct > 100) {
        ESP_LOGE(TAG, "dummy_drive_start: speed_pct %d out of range", speed_pct);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_dummy_running) {
        ESP_LOGW(TAG, "dummy drive already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_dummy_speed_pct = speed_pct;
    s_dummy_running   = true;
    s_state           = NAV_CTRL_RUNNING;

    BaseType_t ok = xTaskCreate(dummy_drive_task, "nav_dummy", 2048, NULL, 4, NULL);
    if (ok != pdPASS) {
        s_dummy_running = false;
        s_state         = NAV_CTRL_IDLE;
        ESP_LOGE(TAG, "Failed to create dummy drive task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Dummy drive started at %d%%", speed_pct);
    return ESP_OK;
}

void nav_controller_dummy_drive_stop(void)
{
    if (!s_dummy_running) return;
    s_dummy_running = false;
    s_state         = NAV_CTRL_IDLE;
    ESP_LOGI(TAG, "Dummy drive stop requested");
}
