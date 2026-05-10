#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NAV_CTRL_IDLE = 0,
    NAV_CTRL_RUNNING,
    NAV_CTRL_PAUSED,
} nav_ctrl_state_t;

const char *nav_ctrl_state_name(nav_ctrl_state_t s);

/**
 * Initialize nav controller subsystem (L1 init, no tasks started yet).
 * Call before nav_controller_start().
 */
esp_err_t nav_controller_init(void);

/**
 * Start background tasks (L1 reflex).
 * Call after WiFi / tool_pwm are ready.
 */
esp_err_t nav_controller_start(void);

nav_ctrl_state_t nav_controller_get_state(void);

/**
 * Phase 5 test hook: drive forward at fixed speed so L1 can be exercised.
 * speed_pct: 1..100 (forward percentage).
 * Returns ESP_ERR_INVALID_STATE if already running.
 */
esp_err_t nav_controller_dummy_drive_start(int speed_pct);

/**
 * Stop dummy drive (safe to call even if not running).
 */
void nav_controller_dummy_drive_stop(void);

bool nav_controller_dummy_is_running(void);

#ifdef __cplusplus
}
#endif
