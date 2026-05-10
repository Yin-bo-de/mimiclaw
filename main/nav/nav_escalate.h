#pragma once

#include "nav/nav_situation.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESC_STUCK = 0,
    ESC_OSCILLATING,
    ESC_NO_PATH,
    ESC_LOST,
    ESC_GOAL_UNREACHABLE,
    ESC_ARRIVED,
    ESC_ABORTED,
    ESC_COUNT
} escalate_kind_t;

/**
 * Initialize escalate subsystem.  Call once before any emit.
 */
void nav_escalate_init(void);

/**
 * Record the channel+chat_id of the nav_goto / nav_goto_waypoint caller.
 * Called by tool_nav before starting a navigation session so escalate events
 * are routed back to the originating user.
 */
void nav_escalate_set_origin(const char *channel, const char *chat_id);

/* ------------------------------------------------------------------ */
/*  Terminal events — no cooldown, call exactly once on state entry     */
/* ------------------------------------------------------------------ */

/**
 * Emit ARRIVED event.  Call when L2 FSM enters L2_ARRIVED.
 * @param sit            Current nav situation.
 * @param duration_s     Trip duration in seconds.
 * @param final_dist_m   Final distance to goal at arrival.
 * @param avoid_events   Total AVOID state entries during the trip.
 */
void nav_escalate_arrived(const nav_situation_t *sit, int64_t duration_s,
                          double final_dist_m, int avoid_events);

/**
 * Emit ABORTED event.  Call when nav is aborted or faulted.
 * @param sit              Current nav situation (may be NULL).
 * @param reason           "user_abort" | "sensors_lost" | "fault".
 * @param dist_remaining_m Remaining metres to goal (0 if unknown).
 */
void nav_escalate_aborted(const nav_situation_t *sit, const char *reason,
                          double dist_remaining_m);

/* ------------------------------------------------------------------ */
/*  Anomaly events — 60 s cooldown per kind                            */
/* ------------------------------------------------------------------ */

/** Emit NO_PATH — called when L2 replan_count > 3. */
void nav_escalate_no_path(const nav_situation_t *sit);

/* ------------------------------------------------------------------ */
/*  Periodic detection — call from L2 task each tick while navigating  */
/* ------------------------------------------------------------------ */

/**
 * Notify escalate of avoid direction changes (for oscillation detection).
 * Call when L2 FSM enters AVOID_LEFT (side=0) or AVOID_RIGHT (side=1).
 */
void nav_escalate_notify_avoid_entry(int side);

/**
 * Run periodic detectors: STUCK, OSCILLATING, LOST, GOAL_UNREACHABLE.
 * Must be called every L2 tick while navigating (not PAUSED/ARRIVED/FAULT).
 * @param sit        Current nav situation.
 * @param avoid_side 0=AVOID_LEFT, 1=AVOID_RIGHT, -1=not in avoid state.
 * @param goal_lat   Active goal latitude.
 * @param goal_lon   Active goal longitude.
 */
void nav_escalate_run_periodic(const nav_situation_t *sit, int avoid_side,
                               double goal_lat, double goal_lon);

#ifdef __cplusplus
}
#endif
