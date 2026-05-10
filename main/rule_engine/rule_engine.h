#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* ── Trigger types ─────────────────────────────────────────────── */

typedef enum {
    RULE_TRIGGER_GPIO_READ = 0,
    RULE_TRIGGER_GPIO_READ_ALL,
    RULE_TRIGGER_INTERVAL,            /* time-based unconditional trigger */
    RULE_TRIGGER_ULTRASONIC_DISTANCE, /* nav_situation distances_cm[channel] */
    RULE_TRIGGER_IMU_TILT,            /* |roll| (ch=0) or |pitch| (ch=1) in degrees */
    RULE_TRIGGER_GPS_DISTANCE_TO,     /* haversine(cur_pos, lat, lon) in meters */
} rule_trigger_type_t;

typedef struct {
    rule_trigger_type_t type;
    int pin;        /* for GPIO_READ */
    int channel;    /* ultrasonic: 0=left,1=front,2=right; imu: 0=roll,1=pitch */
    double lat;     /* reference point for GPS_DISTANCE_TO */
    double lon;
} rule_trigger_t;

/* ── Condition operators ──────────────────────────────────────── */

typedef enum {
    RULE_OP_EQ = 0,
    RULE_OP_NE,
    RULE_OP_GT,
    RULE_OP_LT,
    RULE_OP_GE,
    RULE_OP_LE,
    RULE_OP_ANY_HIGH,   /* only for gpio_read_all */
    RULE_OP_ALL_LOW,    /* only for gpio_read_all */
    RULE_OP_MOD_EQ,     /* counter % value == 0, for interval triggers */
    RULE_OP_MOD_NE,     /* counter % value != 0, for interval triggers */
} rule_condition_op_t;

typedef struct {
    rule_condition_op_t op;
    int value;          /* threshold for numeric comparisons */
} rule_condition_t;

/* ── Action types ──────────────────────────────────────────────── */

typedef enum {
    RULE_ACTION_GPIO_WRITE = 0,
    RULE_ACTION_PWM_SET,
    RULE_ACTION_PWM_RELEASE,
    RULE_ACTION_SCRIPT_RUN,
    RULE_ACTION_ESCALATE,
} rule_action_type_t;

/* Maximum actions per branch */
#define RULE_MAX_ACTIONS 4

typedef struct {
    rule_action_type_t type;
    int pin;            /* gpio_write, pwm_set, pwm_release */
    int value;          /* gpio_write state, pwm_set pulse_us */
    char script_name[32]; /* script_run name */
    char escalate_msg[128]; /* escalate message to agent */
} rule_action_entry_t;

/* ── Rule definition ──────────────────────────────────────────── */

typedef struct {
    char id[9];         /* 8-char hex + null */
    char name[32];
    bool enabled;
    uint32_t interval_s;  /* evaluation interval in seconds */
    uint32_t cooldown_s;  /* minimum seconds between firing */
    int64_t last_eval;    /* epoch of last evaluation */
    int64_t last_fire;    /* epoch of last action fire */
    int fire_count;       /* total times fired (for diagnostics) */
    int counter;          /* increment on each evaluation (for interval triggers) */
    rule_trigger_t trigger;
    rule_condition_t condition;
    int actions_count;
    rule_action_entry_t actions[RULE_MAX_ACTIONS];
    int else_actions_count;
    rule_action_entry_t else_actions[RULE_MAX_ACTIONS];
} rule_t;

/* ── Public API ───────────────────────────────────────────────── */

esp_err_t rule_engine_init(void);
esp_err_t rule_engine_start(void);
void rule_engine_stop(void);

/* Add a new rule (id will be generated). Returns ESP_ERR_NO_MEM if full. */
esp_err_t rule_engine_add(const rule_t *rule);

/* Remove rule by id. */
esp_err_t rule_engine_remove(const char *rule_id);

/* Enable/disable rule by id. */
esp_err_t rule_engine_enable(const char *rule_id);
esp_err_t rule_engine_disable(const char *rule_id);

/* Get read-only access to rules array. */
void rule_engine_list(const rule_t **rules, int *count);

/* Force save rules to disk (normally auto-saved on mutation). */
esp_err_t rule_engine_save(void);
