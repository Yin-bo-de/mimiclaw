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
 * Initialize nav controller subsystem (L1 + L2 init, no tasks started yet).
 */
esp_err_t nav_controller_init(void);

/**
 * Start background tasks (L1 reflex). L2 starts only when a goal is set.
 */
esp_err_t nav_controller_start(void);

nav_ctrl_state_t nav_controller_get_state(void);

/* ------------------------------------------------------------------ */
/*  Goal management (Phase 6)                                           */
/* ------------------------------------------------------------------ */

/**
 * Set an absolute GPS goal and begin autonomous navigation.
 * Starts L2 FSM if not running; updates goal if already running.
 * speed_pct: forward cruise speed 1..100 (default 35).
 */
esp_err_t nav_controller_set_goal(double lat, double lon,
                                   const char *name, int speed_pct);

/**
 * Look up a named waypoint and call nav_controller_set_goal.
 */
esp_err_t nav_controller_set_goal_by_waypoint(const char *name, int speed_pct);

void nav_controller_pause(void);
void nav_controller_resume(void);
void nav_controller_abort(void);

/** Return L2 FSM state name ("CRUISE" / "AVOID_LEFT" / … / "IDLE" when stopped). */
const char *nav_controller_get_l2_state_name(void);

/* ------------------------------------------------------------------ */
/*  Phase 5 test hook (kept for backward compatibility)                 */
/* ------------------------------------------------------------------ */
esp_err_t nav_controller_dummy_drive_start(int speed_pct);
void      nav_controller_dummy_drive_stop(void);
bool      nav_controller_dummy_is_running(void);

#ifdef __cplusplus
}
#endif
