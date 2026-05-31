#include "nav/nav_escalate.h"
#include "nav/nav_planner.h"
#include "bus/message_bus.h"
#include "mimi_config.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "nav_esc";

/* ------------------------------------------------------------------ */
/*  Origin (caller context for routing replies)                         */
/* ------------------------------------------------------------------ */

static char s_channel[16] = {0};
static char s_chat_id[96] = {0};

/* ------------------------------------------------------------------ */
/*  Cooldown tracking                                                   */
/* ------------------------------------------------------------------ */

static int64_t s_last_emit_us[ESC_COUNT];

static bool can_emit(escalate_kind_t k)
{
    /* Terminal events: emit unconditionally (one-shot by design) */
    if (k == ESC_ARRIVED || k == ESC_ABORTED) return true;
    int64_t now = esp_timer_get_time();
    if (now - s_last_emit_us[k] <
        (int64_t)MIMI_NAV_ESCALATE_COOLDOWN_S * 1000000LL) {
        return false;
    }
    s_last_emit_us[k] = now;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Periodic detector state                                             */
/* ------------------------------------------------------------------ */

/* STUCK: GPS anchor position & timestamp */
static double  s_stuck_anchor_lat = 0.0;
static double  s_stuck_anchor_lon = 0.0;
static int64_t s_stuck_anchor_us  = 0;

/* OSCILLATING: ring buffer of last avoid-side entries */
#define OSC_BUF 8
static struct {
    int     side;
    int64_t ts_us;
} s_osc_buf[OSC_BUF];
static int s_osc_head = 0;
static int s_osc_cnt  = 0;  /* filled slots */

/* GOAL_UNREACHABLE: time first observed close but stuck */
static int64_t s_unreachable_first_us = 0;
#define UNREACHABLE_CLOSE_M   5.0
#define UNREACHABLE_WINDOW_S  30

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static cJSON *build_situation_json(const nav_situation_t *sit)
{
    cJSON *s = cJSON_CreateObject();
    if (!sit) return s;

    cJSON_AddNumberToObject(s, "lat",       sit->lat);
    cJSON_AddNumberToObject(s, "lon",       sit->lon);
    cJSON_AddBoolToObject(s,   "gps_fix",   sit->gps_fix);
    cJSON_AddNumberToObject(s, "yaw",       sit->yaw_deg);
    cJSON_AddNumberToObject(s, "speed_mps", sit->speed_mps);

    cJSON *dist = cJSON_CreateObject();
    cJSON_AddNumberToObject(dist, "left",  sit->distances_cm[0]);
    cJSON_AddNumberToObject(dist, "front", sit->distances_cm[1]);
    cJSON_AddNumberToObject(dist, "right", sit->distances_cm[2]);
    cJSON_AddItemToObject(s, "distances", dist);

    return s;
}

/* Map ultrasonic sensor index 0/1/2 → "left"/"front"/"right" for LLM-friendly output. */
static const char *sensor_idx_name(int idx)
{
    switch (idx) {
    case 0:  return "left";
    case 1:  return "front";
    case 2:  return "right";
    default: return "unknown";
    }
}

/* Build a stale_sensors diagnostic array from current situation.
 * A sensor is flagged when distance_valid is false OR last update older than stale_us.
 * Also reports IMU/GPS staleness as separate entries. */
static cJSON *build_stale_sensors_json(const nav_situation_t *sit, int64_t stale_us)
{
    cJSON *arr = cJSON_CreateArray();
    if (!sit) return arr;
    int64_t now = esp_timer_get_time();

    for (int i = 0; i < 3; i++) {
        int64_t age_us = now - sit->distance_ts_us[i];
        bool stale = (!sit->distance_valid[i]) || (age_us >= stale_us);
        if (!stale) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "sensor",   sensor_idx_name(i));
        cJSON_AddNumberToObject(e, "sensor_idx", i);
        cJSON_AddBoolToObject(e,   "valid",    sit->distance_valid[i]);
        cJSON_AddNumberToObject(e, "stale_ms", (double)(age_us / 1000));
        cJSON_AddNumberToObject(e, "last_cm",  sit->distances_cm[i]);
        cJSON_AddItemToArray(arr, e);
    }

    /* IMU staleness (use same threshold) */
    int64_t imu_age = now - sit->imu_ts_us;
    if (!sit->imu_valid || imu_age >= stale_us) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "sensor",   "imu");
        cJSON_AddBoolToObject(e,   "valid",    sit->imu_valid);
        cJSON_AddNumberToObject(e, "stale_ms", (double)(imu_age / 1000));
        cJSON_AddItemToArray(arr, e);
    }

    /* GPS: fix loss is reported elsewhere (LOST event); only include if very stale. */
    int64_t gps_age = now - sit->gps_ts_us;
    if (gps_age >= stale_us) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "sensor",   "gps");
        cJSON_AddBoolToObject(e,   "fix",      sit->gps_fix);
        cJSON_AddNumberToObject(e, "stale_ms", (double)(gps_age / 1000));
        cJSON_AddItemToArray(arr, e);
    }

    return arr;
}

static void emit_to_bus(const char *kind_str, cJSON *root)
{
    if (!root) return;

    cJSON_AddStringToObject(root, "kind", kind_str);

    /* Attach origin */
    cJSON *origin = cJSON_CreateObject();
    cJSON_AddStringToObject(origin, "channel",
                            s_channel[0] ? s_channel : MIMI_CHAN_SYSTEM);
    cJSON_AddStringToObject(origin, "chat_id", s_chat_id);
    cJSON_AddItemToObject(root, "origin", origin);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return;

    mimi_msg_t msg = {0};
    strncpy(msg.channel, s_channel[0] ? s_channel : MIMI_CHAN_SYSTEM,
            sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, s_chat_id, sizeof(msg.chat_id) - 1);
    msg.content = json_str; /* ownership transferred */

    esp_err_t err = message_bus_push_inbound(&msg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "escalate push failed (%s): %s", kind_str, esp_err_to_name(err));
        free(json_str);
    } else {
        ESP_LOGI(TAG, "escalate %s → %s:%s", kind_str, msg.channel, msg.chat_id);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void nav_escalate_init(void)
{
    memset(s_last_emit_us, 0, sizeof(s_last_emit_us));
    memset(s_osc_buf,      0, sizeof(s_osc_buf));
    s_channel[0]       = '\0';
    s_chat_id[0]       = '\0';
    s_stuck_anchor_us  = 0;
    s_osc_head         = 0;
    s_osc_cnt          = 0;
    s_unreachable_first_us = 0;
}

void nav_escalate_set_origin(const char *channel, const char *chat_id)
{
    strncpy(s_channel, channel ? channel : "", sizeof(s_channel) - 1);
    strncpy(s_chat_id, chat_id  ? chat_id  : "", sizeof(s_chat_id)  - 1);
    s_channel[sizeof(s_channel) - 1] = '\0';
    s_chat_id[sizeof(s_chat_id)  - 1] = '\0';
    /* Reset periodic state for new session */
    s_stuck_anchor_us      = 0;
    s_osc_cnt              = 0;
    s_osc_head             = 0;
    s_unreachable_first_us = 0;
    ESP_LOGI(TAG, "Origin set: %s:%s", s_channel, s_chat_id);
}

/* ------------------------------------------------------------------ */
/*  Terminal events                                                     */
/* ------------------------------------------------------------------ */

void nav_escalate_arrived(const nav_situation_t *sit, int64_t duration_s,
                          double final_dist_m, int avoid_events)
{
    if (!can_emit(ESC_ARRIVED)) return;

    cJSON *root = cJSON_CreateObject();

    if (sit && sit->has_goal) {
        cJSON *goal = cJSON_CreateObject();
        cJSON_AddStringToObject(goal, "name", sit->goal_name);
        cJSON_AddNumberToObject(goal, "lat",  sit->goal_lat);
        cJSON_AddNumberToObject(goal, "lon",  sit->goal_lon);
        cJSON_AddItemToObject(root, "goal", goal);
    }

    cJSON *result = cJSON_CreateObject();
    cJSON_AddNumberToObject(result, "final_distance_m", final_dist_m);
    cJSON_AddNumberToObject(result, "duration_s",       (double)duration_s);
    cJSON_AddNumberToObject(result, "avoid_events",     avoid_events);
    cJSON_AddItemToObject(root, "result", result);

    cJSON_AddStringToObject(root, "hint",
        "Notify the user in natural language. "
        "Mention waypoint name, duration, final accuracy.");

    emit_to_bus("ARRIVED", root);
}

void nav_escalate_aborted(const nav_situation_t *sit, const char *reason,
                          double dist_remaining_m, const cJSON *detail)
{
    if (!can_emit(ESC_ABORTED)) return;

    cJSON *root = cJSON_CreateObject();

    if (sit && sit->has_goal) {
        cJSON *goal = cJSON_CreateObject();
        cJSON_AddStringToObject(goal, "name",               sit->goal_name);
        cJSON_AddNumberToObject(goal, "distance_m_remaining", dist_remaining_m);
        cJSON_AddItemToObject(root, "goal", goal);
    }

    cJSON_AddStringToObject(root, "reason", reason ? reason : "unknown");

    /* Auto-attach sensor diagnostics when stop was due to stale sensors.
     * SENSOR_STALE_US lives in nav_l2_fsm.c; we mirror the value (2 s)
     * — keeping it here as a literal so escalate is self-contained. */
    if (reason && strcmp(reason, "sensors_lost") == 0) {
        cJSON_AddItemToObject(root, "stale_sensors",
                              build_stale_sensors_json(sit, 2LL * 1000000LL));
        cJSON_AddItemToObject(root, "situation", build_situation_json(sit));
        cJSON_AddStringToObject(root, "hint",
            "用中文告诉用户：哪些传感器在多少毫秒内未更新数据导致停车，"
            "结合 stale_sensors 数组列出具体传感器名（left/front/right/imu/gps），"
            "建议用户检查传感器接线或重启设备。");
    } else {
        cJSON_AddStringToObject(root, "hint",
            "用中文告诉用户导航已停止，并说明 reason 字段中的原因。");
    }

    /* Merge caller-provided detail object (shallow copy of its children). */
    if (detail && cJSON_IsObject(detail)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, detail) {
            cJSON *copy = cJSON_Duplicate(child, 1);
            if (copy) cJSON_AddItemToObject(root, child->string, copy);
        }
    }

    emit_to_bus("ABORTED", root);
}

/* ------------------------------------------------------------------ */
/*  Anomaly event                                                       */
/* ------------------------------------------------------------------ */

void nav_escalate_emergency_stop(const nav_situation_t *sit,
                                 int sensor_idx,
                                 int distance_cm,
                                 int threshold_cm)
{
    if (!can_emit(ESC_EMERGENCY_STOP)) return;

    cJSON *root = cJSON_CreateObject();

    cJSON *trig = cJSON_CreateObject();
    cJSON_AddStringToObject(trig, "sensor",       sensor_idx_name(sensor_idx));
    cJSON_AddNumberToObject(trig, "sensor_idx",   sensor_idx);
    cJSON_AddNumberToObject(trig, "distance_cm",  distance_cm);
    cJSON_AddNumberToObject(trig, "threshold_cm", threshold_cm);
    cJSON_AddItemToObject(root, "trigger", trig);

    cJSON_AddItemToObject(root, "situation", build_situation_json(sit));

    cJSON_AddStringToObject(root, "hint",
        "用中文告诉用户：trigger.sensor 方向超声波检测到 trigger.distance_cm 厘米"
        "障碍物，已低于紧急刹车阈值 trigger.threshold_cm 厘米，车辆已紧急停止。"
        "请挪开障碍或后退后再继续行驶。");

    emit_to_bus("EMERGENCY_STOP", root);
}

void nav_escalate_no_path(const nav_situation_t *sit,
                          int replan_attempts, int clear_cm_threshold)
{
    if (!can_emit(ESC_NO_PATH)) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "situation", build_situation_json(sit));

    /* Diagnostics: tell LLM exactly why every direction was blocked. */
    cJSON *diag = cJSON_CreateObject();
    cJSON *dist = cJSON_CreateObject();
    if (sit) {
        cJSON_AddNumberToObject(dist, "left",  sit->distances_cm[0]);
        cJSON_AddNumberToObject(dist, "front", sit->distances_cm[1]);
        cJSON_AddNumberToObject(dist, "right", sit->distances_cm[2]);
    }
    cJSON_AddItemToObject(diag,  "distances_cm",      dist);
    cJSON_AddNumberToObject(diag, "clear_threshold_cm", clear_cm_threshold);
    cJSON_AddNumberToObject(diag, "replan_attempts",    replan_attempts);
    cJSON_AddNumberToObject(diag, "max_replan_allowed", 3);
    cJSON_AddItemToObject(root,  "diagnostics", diag);

    cJSON_AddStringToObject(root, "hint",
        "用中文告诉用户：前/左/右三方向的距离值（来自 diagnostics.distances_cm），"
        "均小于通行阈值 clear_threshold_cm，经过 replan_attempts 次重新规划仍无路可走。"
        "调用 nav_abort 并询问用户是否切换路线。");

    emit_to_bus("NO_PATH", root);
}

/* ------------------------------------------------------------------ */
/*  Periodic detection                                                  */
/* ------------------------------------------------------------------ */

void nav_escalate_notify_avoid_entry(int side)
{
    /* Record this avoid-side entry for oscillation tracking */
    s_osc_buf[s_osc_head].side  = side;
    s_osc_buf[s_osc_head].ts_us = esp_timer_get_time();
    s_osc_head = (s_osc_head + 1) % OSC_BUF;
    if (s_osc_cnt < OSC_BUF) s_osc_cnt++;
}

void nav_escalate_run_periodic(const nav_situation_t *sit, int avoid_side,
                               double goal_lat, double goal_lon)
{
    if (!sit) return;
    int64_t now = esp_timer_get_time();

    /* ---- STUCK detector ---- */
    if (sit->gps_fix) {
        if (s_stuck_anchor_us == 0) {
            s_stuck_anchor_lat = sit->lat;
            s_stuck_anchor_lon = sit->lon;
            s_stuck_anchor_us  = now;
        } else {
            double moved = nav_planner_distance_m(s_stuck_anchor_lat, s_stuck_anchor_lon,
                                                  sit->lat,           sit->lon);
            if (moved > 0.30) {
                /* Moved enough — reset anchor */
                s_stuck_anchor_lat = sit->lat;
                s_stuck_anchor_lon = sit->lon;
                s_stuck_anchor_us  = now;
            } else if ((now - s_stuck_anchor_us) > 10LL * 1000000LL) {
                /* < 30 cm in 10 s */
                if (can_emit(ESC_STUCK)) {
                    cJSON *root = cJSON_CreateObject();
                    cJSON_AddStringToObject(root, "detail", "No movement > 30cm in 10s");
                    cJSON_AddItemToObject(root, "situation", build_situation_json(sit));
                    cJSON_AddStringToObject(root, "hint",
                        "Check nav_status. Try nav_manual_step (reverse 800ms) or nav_abort if unsafe.");
                    emit_to_bus("STUCK", root);
                }
                /* Reset anchor so next stuck check starts fresh after cooldown */
                s_stuck_anchor_us = now;
            }
        }
    }

    /* ---- OSCILLATING detector ---- */
    if (s_osc_cnt >= 4) {
        int64_t window_us = 15LL * 1000000LL;
        /* Count direction flips in last 15 s */
        int flips = 0;
        int prev_side = -1;
        for (int i = 0; i < s_osc_cnt; i++) {
            int idx = ((s_osc_head - s_osc_cnt + i + OSC_BUF) % OSC_BUF);
            if ((now - s_osc_buf[idx].ts_us) > window_us) continue;
            if (prev_side != -1 && s_osc_buf[idx].side != prev_side) flips++;
            prev_side = s_osc_buf[idx].side;
        }
        if (flips >= 4) {
            if (can_emit(ESC_OSCILLATING)) {
                cJSON *root = cJSON_CreateObject();
                cJSON_AddNumberToObject(root, "avoid_flip_count", flips);
                cJSON_AddItemToObject(root, "situation", build_situation_json(sit));
                cJSON_AddStringToObject(root, "hint",
                    "Use nav_status. Try nav_manual_step or nav_abort if oscillation persists.");
                emit_to_bus("OSCILLATING", root);
            }
        }
    }

    /* ---- LOST detector ---- */
    if (!sit->gps_fix) {
        if (can_emit(ESC_LOST)) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "detail", "GPS fix lost during navigation");
            cJSON_AddItemToObject(root, "situation", build_situation_json(sit));
            cJSON_AddStringToObject(root, "hint",
                "Re-issue nav_goto with original target. "
                "If GPS fix lost for extended period, nav_abort and notify user.");
            emit_to_bus("LOST", root);
        }
    }

    /* ---- GOAL_UNREACHABLE detector ---- */
    if (sit->gps_fix && goal_lat != 0.0 && goal_lon != 0.0) {
        double dist = nav_planner_distance_m(sit->lat, sit->lon, goal_lat, goal_lon);
        if (dist <= UNREACHABLE_CLOSE_M) {
            if (s_unreachable_first_us == 0) {
                s_unreachable_first_us = now;
            } else if ((now - s_unreachable_first_us) >
                       (int64_t)UNREACHABLE_WINDOW_S * 1000000LL) {
                if (can_emit(ESC_GOAL_UNREACHABLE)) {
                    cJSON *root = cJSON_CreateObject();
                    cJSON_AddNumberToObject(root, "distance_m", dist);
                    cJSON_AddItemToObject(root, "situation", build_situation_json(sit));
                    cJSON_AddStringToObject(root, "hint",
                        "Call nav_abort; within 5m counts as success — "
                        "notify user as ARRIVED-style with final distance.");
                    emit_to_bus("GOAL_UNREACHABLE", root);
                }
                s_unreachable_first_us = now; /* reset so next window restarts */
            }
        } else {
            s_unreachable_first_us = 0; /* not close — reset */
        }
    }

    (void)avoid_side; /* used implicitly via notify_avoid_entry */
}
