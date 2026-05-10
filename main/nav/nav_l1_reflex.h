#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize L1 reflex layer (no-op, config loaded via nav_config).
 */
esp_err_t nav_l1_init(void);

/**
 * Start the 50 Hz L1 reflex FreeRTOS task.
 * Must be called after nav_config_init() and tool_pwm_init().
 */
esp_err_t nav_l1_start(void);

/**
 * Returns true when emergency stop is active (min distance < threshold).
 * Safe to call from any task.
 */
bool nav_l1_is_blocked(void);

#ifdef __cplusplus
}
#endif
