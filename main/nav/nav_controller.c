#include "nav/nav_controller.h"
#include "nav/nav_l1_reflex.h"
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

/* Dummy drive: drives straight at fixed speed; L1 overrides via rc_nav_throttle(0) on block */
static void dummy_drive_task(void *arg)
{
    ESP_LOGI(TAG, "Dummy drive task started at %d%%", s_dummy_speed_pct);
    rc_nav_steer(0);

    while (s_dummy_running) {
        if (!nav_l1_is_blocked()) {
            rc_nav_throttle(s_dummy_speed_pct);
        }
        /* 100ms period — L1 (20ms, prio=7) will always preempt and override when needed */
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    rc_nav_throttle(0);
    rc_nav_steer(0);
    ESP_LOGI(TAG, "Dummy drive task exiting");
    vTaskDelete(NULL);
}

esp_err_t nav_controller_init(void)
{
    esp_err_t err = nav_l1_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nav_l1_init failed: %s", esp_err_to_name(err));
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

esp_err_t nav_controller_dummy_drive_start(int speed_pct)
{
    if (speed_pct < 1 || speed_pct > 100) {
        ESP_LOGE(TAG, "dummy_drive_start: speed_pct %d out of range [1,100]", speed_pct);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_dummy_running) {
        ESP_LOGW(TAG, "dummy drive already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_dummy_speed_pct = speed_pct;
    s_dummy_running = true;
    s_state = NAV_CTRL_RUNNING;

    BaseType_t ok = xTaskCreate(dummy_drive_task, "nav_dummy", 2048, NULL, 4, NULL);
    if (ok != pdPASS) {
        s_dummy_running = false;
        s_state = NAV_CTRL_IDLE;
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
    s_state = NAV_CTRL_IDLE;
    ESP_LOGI(TAG, "Dummy drive stop requested");
}
