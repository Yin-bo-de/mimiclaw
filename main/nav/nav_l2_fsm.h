#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    L2_YAW_BOOTSTRAP = 0, /* Deprecated: magnetometer now provides absolute heading */
    L2_CRUISE,
    L2_AVOID_LEFT,
    L2_AVOID_RIGHT,
    L2_REVERSE,
    L2_REPLAN,
    L2_PAUSED,
    L2_ARRIVED,
    L2_FAULT,
} l2_state_t;

const char *nav_l2_state_name(l2_state_t s);

esp_err_t nav_l2_init(void);

/**
 * Start L2 with a new goal (goal already set in nav_situation by caller).
 * If task is running, resets FSM to CRUISE without restarting the task.
 * speed_pct: 1..100 cruise forward speed.
 */
esp_err_t nav_l2_start(double goal_lat, double goal_lon, const char *goal_name, int speed_pct);

/** Signal L2 task to stop (safe to call even when not running). Non-blocking. */
void nav_l2_stop(void);

void nav_l2_pause(void);
void nav_l2_resume(void);
void nav_l2_abort(void);

l2_state_t  nav_l2_get_state(void);
const char *nav_l2_get_state_name(void);
bool        nav_l2_is_running(void);

int nav_l2_get_avoid_count(void);
int nav_l2_get_replan_count(void);

#ifdef __cplusplus
}
#endif
