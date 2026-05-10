#include "nav/nav_l2_fsm.h"
#include "nav/nav_memory.h"
#include "nav/nav_situation.h"
#include "nav/nav_config.h"
#include "nav/nav_planner.h"
#include "nav/nav_l1_reflex.h"
#include "nav/nav_escalate.h"
#include "tools/tool_pwm.h"
#include "mimi_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <math.h>
#include <string.h>
#include <limits.h>

static const char *TAG = "nav_l2";

/* Sensor stale threshold — enter FAULT if exceeded */
#define SENSOR_STALE_US  (2 * 1000000LL)

/* Minimum time in AVOID before checking "clear" exit to prevent flicker */
#define AVOID_MIN_SETTLE_MS 300

typedef enum {
    L2_CMD_NONE = 0,
    L2_CMD_PAUSE,
    L2_CMD_RESUME,
    L2_CMD_ABORT,
    L2_CMD_NEW_GOAL,
} l2_cmd_t;

static volatile l2_state_t s_state        = L2_FAULT;
static volatile l2_cmd_t   s_cmd          = L2_CMD_NONE;
static volatile bool        s_running     = false;
static volatile bool        s_task_alive  = false;

static l2_state_t s_prev_state            = L2_CRUISE;
static int64_t    s_state_enter_us        = 0;

static int   s_avoid_count   = 0;
static int   s_replan_count  = 0;
static int   s_cruise_speed  = 35; /* overridden by nav_l2_start */

static double  s_goal_lat  = 0.0;
static double  s_goal_lon  = 0.0;
static char    s_goal_name[64] = {0};
static int64_t s_trip_start_us = 0; /* set when trip begins, used by ARRIVED escalate */

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

const char *nav_l2_state_name(l2_state_t s)
{
    switch (s) {
    case L2_CRUISE:      return "CRUISE";
    case L2_AVOID_LEFT:  return "AVOID_LEFT";
    case L2_AVOID_RIGHT: return "AVOID_RIGHT";
    case L2_REVERSE:     return "REVERSE";
    case L2_REPLAN:      return "REPLAN";
    case L2_PAUSED:      return "PAUSED";
    case L2_ARRIVED:     return "ARRIVED";
    case L2_FAULT:       return "FAULT";
    default:             return "UNKNOWN";
    }
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int min_valid_distance(const nav_situation_t *sit)
{
    int64_t now = esp_timer_get_time();
    int min = INT_MAX;
    for (int i = 0; i < 3; i++) {
        if (sit->distance_valid[i] &&
            (now - sit->distance_ts_us[i]) < SENSOR_STALE_US) {
            if (sit->distances_cm[i] < min) min = sit->distances_cm[i];
        }
    }
    return (min == INT_MAX) ? 400 : min;
}

/* Returns true if all three ultrasonic channels have fresh data. */
static bool sensors_ok(const nav_situation_t *sit)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < 3; i++) {
        if (!sit->distance_valid[i] ||
            (now - sit->distance_ts_us[i]) >= SENSOR_STALE_US) {
            return false;
        }
    }
    return true;
}

/* Score the two side options; returns 0 for left, 1 for right. */
static int score_sides(const nav_situation_t *sit, double bearing_to_goal)
{
    /* clearance term: normalise to 200 cm max scale */
    float cl = (float)sit->distances_cm[0] / 200.0f;
    float cr = (float)sit->distances_cm[2] / 200.0f;

    /* heading term: bearing offset ±30° from goal bearing */
    double left_bear  = fmod(bearing_to_goal - 30.0 + 360.0, 360.0);
    double right_bear = fmod(bearing_to_goal + 30.0,          360.0);
    double left_err   = nav_planner_heading_error_deg(sit->yaw_deg, left_bear);
    double right_err  = nav_planner_heading_error_deg(sit->yaw_deg, right_bear);
    float hl = -(float)fabs(left_err)  / 90.0f;
    float hr = -(float)fabs(right_err) / 90.0f;

    /* memory penalty */
    float pl = nav_memory_penalty_for_side(0, sit->lat, sit->lon);
    float pr = nav_memory_penalty_for_side(1, sit->lat, sit->lon);

    float score_left  = cl + hl - pl;
    float score_right = cr + hr - pr;

    ESP_LOGD(TAG, "Score L=%.2f(c%.2f h%.2f p%.2f) R=%.2f(c%.2f h%.2f p%.2f)",
             score_left, cl, hl, pl, score_right, cr, hr, pr);

    return (score_left >= score_right) ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/*  State machine transitions                                           */
/* ------------------------------------------------------------------ */

static void enter_state(l2_state_t new_state);

static void on_leave_avoid(l2_state_t to)
{
    int64_t dur_ms = (esp_timer_get_time() - s_state_enter_us) / 1000LL;
    bool succeeded = (to == L2_CRUISE);
    nav_memory_mark_last_result(succeeded, (uint32_t)dur_ms);
    if (succeeded) {
        ESP_LOGI(TAG, "AVOID succeeded after %lld ms", (long long)dur_ms);
    } else {
        ESP_LOGW(TAG, "AVOID failed after %lld ms → %s", (long long)dur_ms, nav_l2_state_name(to));
    }
}

static void enter_state(l2_state_t new_state)
{
    l2_state_t cur = s_state;

    /* Mark avoid memory on leaving AVOID state */
    if (cur == L2_AVOID_LEFT || cur == L2_AVOID_RIGHT) {
        on_leave_avoid(new_state);
    }

    /* Track replan count; cap to avoid infinite loop */
    if (new_state == L2_REPLAN) {
        s_replan_count++;
        if (s_replan_count > 3) {
            ESP_LOGW(TAG, "NO_PATH: replan_count=%d — entering FAULT", s_replan_count);
            nav_situation_t tmp_sit;
            nav_situation_get(&tmp_sit);
            nav_escalate_no_path(&tmp_sit);
            new_state = L2_FAULT;
        }
    }

    if (new_state != cur) {
        ESP_LOGI(TAG, "%s → %s", nav_l2_state_name(cur), nav_l2_state_name(new_state));
        /* Notify escalate so it can track oscillation */
        if (new_state == L2_AVOID_LEFT)  nav_escalate_notify_avoid_entry(0);
        if (new_state == L2_AVOID_RIGHT) nav_escalate_notify_avoid_entry(1);
    }
    s_state          = new_state;
    s_state_enter_us = esp_timer_get_time();
}

/* ------------------------------------------------------------------ */
/*  Per-state behaviour                                                 */
/* ------------------------------------------------------------------ */

static void fsm_cruise(const nav_situation_t *sit, const nav_config_t *cfg)
{
    /* Check arrival */
    if (sit->gps_fix && sit->has_goal) {
        double dist = nav_planner_distance_m(sit->lat, sit->lon,
                                              s_goal_lat, s_goal_lon);
        if (dist <= (double)cfg->arrival_radius_m) {
            int64_t duration_s = (esp_timer_get_time() - s_trip_start_us) / 1000000LL;
            nav_escalate_arrived(sit, duration_s, dist, s_avoid_count);
            enter_state(L2_ARRIVED);
            rc_nav_throttle(0);
            rc_nav_steer(0);
            ESP_LOGI(TAG, "ARRIVED at '%s' (dist=%.2f m, dur=%llds)",
                     s_goal_name, dist, (long long)duration_s);
            return;
        }
    }

    /* Compute steer toward goal */
    int steer = 0;
    if (sit->gps_fix && sit->has_goal) {
        double bearing = nav_planner_bearing_deg(sit->lat, sit->lon,
                                                  s_goal_lat, s_goal_lon);
        double herr = nav_planner_heading_error_deg(sit->yaw_deg, bearing);
        steer = clamp_int((int)(cfg->heading_kp * herr),
                          -cfg->heading_max_steer_pct,
                           cfg->heading_max_steer_pct);
        ESP_LOGI(TAG, "NAV: goal_bearing=%.1f° yaw=%.1f° error=%.1f° steer=%d",
                 bearing, sit->yaw_deg, herr, steer);
    }

    /* Check for obstacle */
    int min_d = min_valid_distance(sit);
    if (min_d < cfg->avoid_trigger_cm) {
        double bearing = sit->gps_fix && sit->has_goal
            ? nav_planner_bearing_deg(sit->lat, sit->lon, s_goal_lat, s_goal_lon)
            : sit->yaw_deg;  /* fallback: current heading */
        int side = score_sides(sit, bearing);
        nav_memory_record_attempt(sit->lat, sit->lon, side, sit->distances_cm[1]);
        s_avoid_count++;
        enter_state(side == 0 ? L2_AVOID_LEFT : L2_AVOID_RIGHT);
        return;
    }

    rc_nav_steer(steer);
    if (!nav_l1_is_blocked()) {
        rc_nav_throttle(s_cruise_speed);
    }
}

static void fsm_avoid(const nav_situation_t *sit, const nav_config_t *cfg, int side)
{
    int64_t elapsed_ms = (esp_timer_get_time() - s_state_enter_us) / 1000LL;

    /* Hard-left or hard-right */
    rc_nav_steer(side == 0 ? -100 : 100);
    if (!nav_l1_is_blocked()) {
        rc_nav_throttle(cfg->avoid_speed_pct);
    }

    /* Emergency reverse if front is too close */
    if (sit->distances_cm[1] < cfg->emergency_reverse_cm) {
        enter_state(L2_REVERSE);
        return;
    }

    /* Success: front clear after min settle time */
    if (elapsed_ms > AVOID_MIN_SETTLE_MS &&
        sit->distances_cm[1] >= cfg->clear_cm) {
        enter_state(L2_CRUISE);
        return;
    }

    /* Timeout → REPLAN */
    if (elapsed_ms > cfg->avoid_max_ms) {
        enter_state(L2_REPLAN);
    }
}

static void fsm_reverse(const nav_situation_t *sit, const nav_config_t *cfg)
{
    (void)sit;
    int64_t elapsed_ms = (esp_timer_get_time() - s_state_enter_us) / 1000LL;

    rc_nav_steer(0);
    if (!nav_l1_is_blocked()) {
        rc_nav_throttle(-cfg->reverse_speed_pct);
    }

    if (elapsed_ms >= cfg->reverse_ms) {
        enter_state(L2_REPLAN);
    }
}

static void fsm_replan(const nav_situation_t *sit, const nav_config_t *cfg)
{
    int64_t elapsed_ms = (esp_timer_get_time() - s_state_enter_us) / 1000LL;

    /* Sweep steer: -100 → +100 in first half, +100 → -100 in second half */
    float progress = (float)elapsed_ms / (float)cfg->replan_ms;
    if (progress > 1.0f) progress = 1.0f;
    int sweep;
    if (progress < 0.5f) {
        sweep = (int)(-100.0f + 200.0f * (progress / 0.5f));
    } else {
        sweep = (int)(100.0f - 200.0f * ((progress - 0.5f) / 0.5f));
    }
    rc_nav_steer(sweep);
    rc_nav_throttle(0);

    /* Found clear side → route to AVOID */
    if (sit->distances_cm[1] >= cfg->clear_cm) {
        /* Front clear — go back to CRUISE */
        s_replan_count = 0;  /* successful replan resets count */
        enter_state(L2_CRUISE);
        return;
    }
    if (sit->distances_cm[0] >= cfg->clear_cm) {
        s_replan_count = 0;
        nav_memory_record_attempt(sit->lat, sit->lon, 0, sit->distances_cm[1]);
        s_avoid_count++;
        enter_state(L2_AVOID_LEFT);
        return;
    }
    if (sit->distances_cm[2] >= cfg->clear_cm) {
        s_replan_count = 0;
        nav_memory_record_attempt(sit->lat, sit->lon, 1, sit->distances_cm[1]);
        s_avoid_count++;
        enter_state(L2_AVOID_RIGHT);
        return;
    }

    /* Sweep complete without finding clear path */
    if (elapsed_ms >= cfg->replan_ms) {
        /* enter_state(L2_REPLAN) increments replan_count and switches to FAULT at >3 */
        enter_state(L2_REPLAN);
    }
}

/* ------------------------------------------------------------------ */
/*  L2 FreeRTOS task                                                    */
/* ------------------------------------------------------------------ */

static void l2_task(void *arg)
{
    const nav_config_t *cfg = nav_config_get();
    TickType_t last_wake    = xTaskGetTickCount();

    ESP_LOGI(TAG, "L2 task started — goal '%s' speed=%d%%",
             s_goal_name, s_cruise_speed);

    /* Reset FSM on fresh start */
    s_avoid_count  = 0;
    s_replan_count = 0;
    enter_state(L2_CRUISE);

    while (s_running) {
        nav_situation_t sit;
        nav_situation_get(&sit);

        /* Process any pending command */
        l2_cmd_t cmd = s_cmd;
        if (cmd != L2_CMD_NONE) {
            s_cmd = L2_CMD_NONE;
            switch (cmd) {
            case L2_CMD_PAUSE:
                if (s_state != L2_ARRIVED && s_state != L2_FAULT) {
                    s_prev_state = s_state;
                    enter_state(L2_PAUSED);
                }
                break;
            case L2_CMD_RESUME:
                if (s_state == L2_PAUSED) {
                    enter_state(s_prev_state);
                }
                break;
            case L2_CMD_ABORT: {
                double dist_rem = 0.0;
                if (sit.gps_fix && sit.has_goal) {
                    dist_rem = nav_planner_distance_m(sit.lat, sit.lon,
                                                      s_goal_lat, s_goal_lon);
                }
                nav_escalate_aborted(&sit, "user_abort", dist_rem);
                enter_state(L2_FAULT);
                rc_nav_throttle(0);
                rc_nav_steer(0);
                break;
            }
            case L2_CMD_NEW_GOAL:
                s_avoid_count  = 0;
                s_replan_count = 0;
                enter_state(L2_CRUISE);
                break;
            default:
                break;
            }
        }

        /* Sensor staleness guard (not in terminal / paused states) */
        if (s_state != L2_PAUSED &&
            s_state != L2_ARRIVED &&
            s_state != L2_FAULT) {
            if (!sensors_ok(&sit)) {
                ESP_LOGE(TAG, "Sensor stale — FAULT");
                nav_escalate_aborted(&sit, "sensors_lost", 0.0);
                enter_state(L2_FAULT);
                rc_nav_throttle(0);
                rc_nav_steer(0);
            }
        }

        /* Run current state */
        switch (s_state) {
        case L2_CRUISE:
            fsm_cruise(&sit, cfg);
            break;
        case L2_AVOID_LEFT:
            fsm_avoid(&sit, cfg, 0);
            break;
        case L2_AVOID_RIGHT:
            fsm_avoid(&sit, cfg, 1);
            break;
        case L2_REVERSE:
            fsm_reverse(&sit, cfg);
            break;
        case L2_REPLAN:
            fsm_replan(&sit, cfg);
            break;
        case L2_PAUSED:
            rc_nav_throttle(0);
            break;
        case L2_ARRIVED:
        case L2_FAULT:
            rc_nav_throttle(0);
            rc_nav_steer(0);
            break;
        }

        /* Periodic escalate detectors (run while actively navigating) */
        if (s_state == L2_CRUISE   || s_state == L2_AVOID_LEFT  ||
            s_state == L2_AVOID_RIGHT || s_state == L2_REVERSE  ||
            s_state == L2_REPLAN) {
            int avoid_side = (s_state == L2_AVOID_LEFT)  ? 0 :
                             (s_state == L2_AVOID_RIGHT) ? 1 : -1;
            nav_escalate_run_periodic(&sit, avoid_side, s_goal_lat, s_goal_lon);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MIMI_NAV_L2_PERIOD_MS));
    }

    /* Clean up on exit */
    rc_nav_throttle(0);
    rc_nav_steer(0);
    s_task_alive = false;
    ESP_LOGI(TAG, "L2 task exiting");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t nav_l2_init(void)
{
    nav_memory_init();
    nav_escalate_init();
    s_state         = L2_FAULT;
    s_running       = false;
    s_cmd           = L2_CMD_NONE;
    s_trip_start_us = 0;
    ESP_LOGI(TAG, "nav_l2 initialized");
    return ESP_OK;
}

esp_err_t nav_l2_start(double goal_lat, double goal_lon, const char *goal_name, int speed_pct)
{
    if (speed_pct < 1 || speed_pct > 100) {
        ESP_LOGE(TAG, "speed_pct %d out of range", speed_pct);
        return ESP_ERR_INVALID_ARG;
    }

    s_goal_lat      = goal_lat;
    s_goal_lon      = goal_lon;
    s_cruise_speed  = speed_pct;
    s_trip_start_us = esp_timer_get_time();
    strncpy(s_goal_name, goal_name ? goal_name : "", sizeof(s_goal_name) - 1);
    s_goal_name[sizeof(s_goal_name) - 1] = '\0';

    if (s_task_alive) {
        /* Signal running task to reset to CRUISE with new goal */
        s_cmd = L2_CMD_NEW_GOAL;
        ESP_LOGI(TAG, "L2 goal updated → '%s' at (%.6f, %.6f) speed=%d%%",
                 s_goal_name, goal_lat, goal_lon, speed_pct);
        return ESP_OK;
    }

    s_running    = true;
    s_task_alive = true;
    BaseType_t ok = xTaskCreatePinnedToCore(
        l2_task, "nav_l2",
        MIMI_NAV_L2_STACK, NULL,
        MIMI_NAV_L2_PRIO, NULL,
        MIMI_NAV_L2_CORE);
    if (ok != pdPASS) {
        s_running    = false;
        s_task_alive = false;
        ESP_LOGE(TAG, "Failed to create L2 task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "L2 started → '%s' at (%.6f, %.6f) speed=%d%%",
             s_goal_name, goal_lat, goal_lon, speed_pct);
    return ESP_OK;
}

void nav_l2_stop(void)
{
    if (!s_task_alive) return;
    s_running = false;
    ESP_LOGI(TAG, "L2 stop requested");
}

void nav_l2_pause(void)  { s_cmd = L2_CMD_PAUSE; }
void nav_l2_resume(void) { s_cmd = L2_CMD_RESUME; }
void nav_l2_abort(void)  { s_cmd = L2_CMD_ABORT; }

l2_state_t  nav_l2_get_state(void)      { return s_state; }
const char *nav_l2_get_state_name(void) { return nav_l2_state_name(s_state); }
bool        nav_l2_is_running(void)     { return s_task_alive; }
int         nav_l2_get_avoid_count(void)  { return s_avoid_count; }
int         nav_l2_get_replan_count(void) { return s_replan_count; }
