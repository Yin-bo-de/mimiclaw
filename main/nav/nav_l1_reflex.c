#include "nav/nav_l1_reflex.h"
#include "nav/nav_situation.h"
#include "nav/nav_config.h"
#include "nav/nav_escalate.h"
#include "tools/tool_pwm.h"
#include "mimi_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <limits.h>
#include <stdbool.h>

static const char *TAG = "nav_l1";

/* Sensor reading older than this is treated as stale — use max range assumption */
#define STALE_US  (500 * 1000LL)

static volatile bool s_blocked = false;

bool nav_l1_is_blocked(void)
{
    return s_blocked;
}

static void l1_task(void *arg)
{
    const nav_config_t *cfg = nav_config_get();
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        nav_situation_t sit;
        nav_situation_get(&sit);

        int64_t now = esp_timer_get_time();
        int min_d = INT_MAX;
        int min_idx = -1;          /* which sensor produced min_d */
        bool any_valid = false;

        for (int i = 0; i < 3; i++) {
            if (sit.distance_valid[i] &&
                (now - sit.distance_ts_us[i]) < STALE_US) {
                if (sit.distances_cm[i] < min_d) {
                    min_d = sit.distances_cm[i];
                    min_idx = i;
                }
                any_valid = true;
            }
        }

        if (any_valid && min_d < cfg->emergency_stop_cm) {
            if (!s_blocked) {
                const char *sname = (min_idx == 0 ? "left" :
                                     min_idx == 1 ? "front" :
                                     min_idx == 2 ? "right" : "unknown");
                ESP_LOGW(TAG, "EMERGENCY STOP: %s sensor min_d=%d cm (threshold=%d cm)",
                         sname, min_d, cfg->emergency_stop_cm);
                /* Escalate to LLM so user learns *why* the car stopped. */
                nav_escalate_emergency_stop(&sit, min_idx, min_d, cfg->emergency_stop_cm);
            }
            s_blocked = true;
            /* Force throttle to zero every tick while blocked — overrides L2/dummy */
            rc_nav_throttle(0);
        } else {
            if (s_blocked) {
                if (any_valid && min_d < INT_MAX) {
                    ESP_LOGI(TAG, "Obstacle cleared — releasing emergency stop (min_d=%d cm)", min_d);
                } else {
                    ESP_LOGI(TAG, "Obstacle cleared — releasing emergency stop (no valid sensor)");
                }
            }
            s_blocked = false;
            /* Not blocked: do nothing, let higher layers control throttle */
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MIMI_NAV_L1_PERIOD_MS));
    }
}

esp_err_t nav_l1_init(void)
{
    /* nav_config must have been loaded already (done by tool_nav_init) */
    return ESP_OK;
}

esp_err_t nav_l1_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
        l1_task, "nav_l1",
        MIMI_NAV_L1_STACK, NULL,
        MIMI_NAV_L1_PRIO, NULL,
        MIMI_NAV_L1_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create L1 task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "L1 reflex started (prio=%d core=%d period=%d ms threshold=%d cm)",
             MIMI_NAV_L1_PRIO, MIMI_NAV_L1_CORE, MIMI_NAV_L1_PERIOD_MS,
             nav_config_get()->emergency_stop_cm);
    return ESP_OK;
}
