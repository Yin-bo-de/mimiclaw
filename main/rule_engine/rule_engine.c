#include "rule_engine/rule_engine.h"
#include "mimi_config.h"
#include "tools/gpio_policy.h"
#include "tools/tool_registry.h"
#include "bus/message_bus.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nav/nav_situation.h"
#include "nav/nav_planner.h"
#include "esp_timer.h"
#include <math.h>

static const char *TAG = "rule_engine";

#define MAX_RULES  MIMI_RULE_MAX_RULES

static rule_t s_rules[MAX_RULES];
static int s_rule_count = 0;
static TaskHandle_t s_rule_task = NULL;
static volatile bool s_running = false;

/* ── Helpers ───────────────────────────────────────────────────── */

static void generate_id(char *buf)
{
    uint32_t r = esp_random();
    snprintf(buf, 9, "%08x", (unsigned int)r);
}

static const char *trigger_type_str(rule_trigger_type_t t)
{
    switch (t) {
        case RULE_TRIGGER_GPIO_READ: return "gpio_read";
        case RULE_TRIGGER_GPIO_READ_ALL: return "gpio_read_all";
        case RULE_TRIGGER_INTERVAL: return "interval";
        case RULE_TRIGGER_ULTRASONIC_DISTANCE:    return "ultrasonic_distance";
        case RULE_TRIGGER_IMU_TILT:               return "imu_tilt";
        case RULE_TRIGGER_GPS_DISTANCE_TO:        return "gps_distance_to";
        default: return "unknown";
    }
}

static const char *op_str(rule_condition_op_t op)
{
    switch (op) {
        case RULE_OP_EQ: return "==";
        case RULE_OP_NE: return "!=";
        case RULE_OP_GT: return ">";
        case RULE_OP_LT: return "<";
        case RULE_OP_GE: return ">=";
        case RULE_OP_LE: return "<=";
        case RULE_OP_ANY_HIGH: return "any_high";
        case RULE_OP_ALL_LOW: return "all_low";
        case RULE_OP_MOD_EQ: return "mod_eq";
        case RULE_OP_MOD_NE: return "mod_ne";
        default: return "?";
    }
}

static const char *action_type_str(rule_action_type_t t)
{
    switch (t) {
        case RULE_ACTION_GPIO_WRITE: return "gpio_write";
        case RULE_ACTION_PWM_SET: return "pwm_set";
        case RULE_ACTION_PWM_RELEASE: return "pwm_release";
        case RULE_ACTION_SCRIPT_RUN: return "script_run";
        case RULE_ACTION_ESCALATE: return "escalate";
        default: return "unknown";
    }
}

/* ── Trigger evaluation ────────────────────────────────────────── */

static bool eval_trigger_gpio_read(int pin, int *out_value)
{
    if (!gpio_policy_pin_is_allowed(pin)) return false;
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    *out_value = gpio_get_level(pin);
    return true;
}

static bool eval_trigger_gpio_read_all(int *out_value)
{
    bool any_high = false;

    if (MIMI_GPIO_ALLOWED_CSV[0] != '\0') {
        const char *p = MIMI_GPIO_ALLOWED_CSV;
        while (*p != '\0') {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (*p == '\0') break;
            char *end = NULL;
            long val = strtol(p, &end, 10);
            if (end == p) { while (*p != '\0' && *p != ',') p++; continue; }
            if (gpio_policy_pin_is_allowed((int)val)) {
                gpio_set_direction((int)val, GPIO_MODE_INPUT);
                int lvl = gpio_get_level((int)val);
                if (lvl) any_high = true;
            }
            p = end;
        }
    } else {
        for (int pin = MIMI_GPIO_MIN_PIN; pin <= MIMI_GPIO_MAX_PIN; pin++) {
            if (!gpio_policy_pin_is_allowed(pin)) continue;
            gpio_set_direction(pin, GPIO_MODE_INPUT);
            int lvl = gpio_get_level(pin);
            if (lvl) any_high = true;
        }
    }
    *out_value = any_high ? 1 : 0;
    return true;
}

static bool eval_trigger_ultrasonic_distance(int channel, int *out_value)
{
    if (channel < 0 || channel > 2) return false;
    nav_situation_t sit;
    nav_situation_get(&sit);
    if (!sit.distance_valid[channel]) return false;
    int64_t age_us = esp_timer_get_time() - sit.distance_ts_us[channel];
    if (age_us > 500000LL) return false;
    *out_value = sit.distances_cm[channel];
    return true;
}

static bool eval_trigger_imu_tilt(int channel, int *out_value)
{
    if (channel < 0 || channel > 1) return false;
    nav_situation_t sit;
    nav_situation_get(&sit);
    if (!sit.imu_valid) return false;
    int64_t age_us = esp_timer_get_time() - sit.imu_ts_us;
    if (age_us > 500000LL) return false;
    float deg = (channel == 1) ? sit.pitch_deg : sit.roll_deg;
    *out_value = (int)fabsf(deg);
    return true;
}

static bool eval_trigger_gps_distance(double ref_lat, double ref_lon, int *out_value)
{
    nav_situation_t sit;
    nav_situation_get(&sit);
    if (!sit.gps_fix) return false;
    int64_t age_us = esp_timer_get_time() - sit.gps_ts_us;
    if (age_us > 5000000LL) return false;
    *out_value = (int)nav_planner_distance_m(sit.lat, sit.lon, ref_lat, ref_lon);
    return true;
}

/* ── Condition check ───────────────────────────────────────────── */

static bool check_condition(rule_condition_t *cond, int value)
{
    switch (cond->op) {
        case RULE_OP_EQ:       return value == cond->value;
        case RULE_OP_NE:       return value != cond->value;
        case RULE_OP_GT:       return value > cond->value;
        case RULE_OP_LT:       return value < cond->value;
        case RULE_OP_GE:       return value >= cond->value;
        case RULE_OP_LE:       return value <= cond->value;
        case RULE_OP_ANY_HIGH: return value != 0;  /* value = 1 if any pin high */
        case RULE_OP_ALL_LOW:  return value == 0;  /* value = 1 if any pin high */
        case RULE_OP_MOD_EQ:   return cond->value != 0 && (value % cond->value) == 0;
        case RULE_OP_MOD_NE:   return cond->value != 0 && (value % cond->value) != 0;
        default:               return false;
    }
}

/* ── Action execution ──────────────────────────────────────────── */

static void execute_single_action(rule_action_entry_t *act, char *log_buf, size_t log_size)
{
    switch (act->type) {
        case RULE_ACTION_GPIO_WRITE: {
            if (!gpio_policy_pin_is_allowed(act->pin)) {
                snprintf(log_buf, log_size, "gpio_write GPIO%d forbidden", act->pin);
                return;
            }
            gpio_set_direction(act->pin, GPIO_MODE_OUTPUT);
            gpio_set_level(act->pin, act->value ? 1 : 0);
            snprintf(log_buf, log_size, "gpio_write GPIO%d -> %s", act->pin, act->value ? "HIGH" : "LOW");
            break;
        }
        case RULE_ACTION_PWM_SET: {
            char input[64];
            snprintf(input, sizeof(input), "{\"gpio\":%d,\"pulse_us\":%d}", act->pin, act->value);
            char out[128];
            esp_err_t err = tool_registry_execute("pwm_set", input, out, sizeof(out));
            snprintf(log_buf, log_size, "pwm_set GPIO%d %dus: %s", act->pin, act->value, err == ESP_OK ? "ok" : out);
            break;
        }
        case RULE_ACTION_PWM_RELEASE: {
            char input[32];
            snprintf(input, sizeof(input), "{\"gpio\":%d}", act->pin);
            char out[128];
            esp_err_t err = tool_registry_execute("pwm_release", input, out, sizeof(out));
            snprintf(log_buf, log_size, "pwm_release GPIO%d: %s", act->pin, err == ESP_OK ? "ok" : out);
            break;
        }
        case RULE_ACTION_SCRIPT_RUN: {
            char input[64];
            snprintf(input, sizeof(input), "{\"name\":\"%s\"}", act->script_name);
            char out[256];
            esp_err_t err = tool_registry_execute("script_run", input, out, sizeof(out));
            snprintf(log_buf, log_size, "script_run '%s': %s", act->script_name, err == ESP_OK ? "ok" : out);
            break;
        }
        case RULE_ACTION_ESCALATE: {
            mimi_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            strncpy(msg.channel, MIMI_CHAN_SYSTEM, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, "rule_engine", sizeof(msg.chat_id) - 1);
            msg.content = strdup(act->escalate_msg[0] ? act->escalate_msg : "Rule escalation triggered");
            if (msg.content) {
                esp_err_t err = message_bus_push_inbound(&msg);
                if (err != ESP_OK) {
                    free(msg.content);
                    snprintf(log_buf, log_size, "escalate: bus push failed");
                } else {
                    snprintf(log_buf, log_size, "escalate: pushed to agent");
                }
            } else {
                snprintf(log_buf, log_size, "escalate: out of memory");
            }
            break;
        }
        default:
            snprintf(log_buf, log_size, "unknown action type %d", act->type);
            break;
    }
}

static void execute_actions(rule_t *rule, bool condition_met)
{
    int count = condition_met ? rule->actions_count : rule->else_actions_count;
    rule_action_entry_t *acts = condition_met ? rule->actions : rule->else_actions;
    const char *branch = condition_met ? "then" : "else";

    for (int i = 0; i < count; i++) {
        char log[256];
        execute_single_action(&acts[i], log, sizeof(log));
        ESP_LOGI(TAG, "Rule '%s' [%s] action %d/%d: %s", rule->name, branch, i + 1, count, log);
    }
}

/* ── Rule evaluation ───────────────────────────────────────────── */

static void evaluate_rule(rule_t *rule)
{
    if (!rule->enabled) return;

    time_t now = time(NULL);

    /* Respect evaluation interval */
    if (rule->last_eval > 0 && (now - rule->last_eval) < (int64_t)rule->interval_s) {
        return;
    }
    rule->last_eval = now;

    /* Read trigger */
    int trigger_value = 0;
    bool trigger_ok = false;
    bool is_interval = (rule->trigger.type == RULE_TRIGGER_INTERVAL);

    switch (rule->trigger.type) {
        case RULE_TRIGGER_GPIO_READ:
            trigger_ok = eval_trigger_gpio_read(rule->trigger.pin, &trigger_value);
            break;
        case RULE_TRIGGER_GPIO_READ_ALL:
            trigger_ok = eval_trigger_gpio_read_all(&trigger_value);
            break;
        case RULE_TRIGGER_INTERVAL:
            trigger_ok = true;
            trigger_value = rule->counter;
            break;
        case RULE_TRIGGER_ULTRASONIC_DISTANCE:
            trigger_ok = eval_trigger_ultrasonic_distance(rule->trigger.channel, &trigger_value);
            break;
        case RULE_TRIGGER_IMU_TILT:
            trigger_ok = eval_trigger_imu_tilt(rule->trigger.channel, &trigger_value);
            break;
        case RULE_TRIGGER_GPS_DISTANCE_TO:
            trigger_ok = eval_trigger_gps_distance(rule->trigger.lat, rule->trigger.lon, &trigger_value);
            break;
        default:
            ESP_LOGW(TAG, "Rule '%s': unknown trigger type %d", rule->name, rule->trigger.type);
            return;
    }

    if (!trigger_ok) {
        ESP_LOGW(TAG, "Rule '%s': trigger evaluation failed", rule->name);
        return;
    }

    /* Evaluate condition */
    bool condition_met = check_condition(&rule->condition, trigger_value);

    ESP_LOGD(TAG, "Rule '%s': trigger=%d condition=%s",
             rule->name, trigger_value, condition_met ? "true" : "false");

    /* Check cooldown (only for actions branch; else_actions bypass cooldown) */
    if (condition_met) {
        if (rule->cooldown_s > 0 && rule->last_fire > 0 &&
            (now - rule->last_fire) < (int64_t)rule->cooldown_s) {
            ESP_LOGD(TAG, "Rule '%s': in cooldown (%llds left)",
                     rule->name, (long long)(rule->cooldown_s - (now - rule->last_fire)));
            if (is_interval) rule->counter++;  /* still increment counter even in cooldown */
            return;
        }

        rule->last_fire = now;
        rule->fire_count++;
        ESP_LOGI(TAG, "Rule '%s' FIRE (count=%d, value=%d)",
                 rule->name, rule->fire_count, trigger_value);
    } else {
        /* else_actions don't have cooldown */
    }

    /* Execute actions */
    execute_actions(rule, condition_met);

    /* Increment counter after each evaluation for interval triggers */
    if (is_interval) {
        rule->counter++;
    }
}

/* ── FreeRTOS task ─────────────────────────────────────────────── */

static void rule_engine_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Rule engine task started");

    while (s_running) {
        for (int i = 0; i < s_rule_count; i++) {
            evaluate_rule(&s_rules[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); /* 1-second polling loop */
    }

    s_rule_task = NULL;
    vTaskDelete(NULL);
}

/* ── Persistence ───────────────────────────────────────────────── */

static rule_trigger_type_t parse_trigger_type(const char *s)
{
    if (strcmp(s, "gpio_read") == 0) return RULE_TRIGGER_GPIO_READ;
    if (strcmp(s, "gpio_read_all") == 0) return RULE_TRIGGER_GPIO_READ_ALL;
    if (strcmp(s, "interval") == 0) return RULE_TRIGGER_INTERVAL;
    if (strcmp(s, "ultrasonic_distance") == 0) return RULE_TRIGGER_ULTRASONIC_DISTANCE;
    if (strcmp(s, "imu_tilt") == 0) return RULE_TRIGGER_IMU_TILT;
    if (strcmp(s, "gps_distance_to") == 0) return RULE_TRIGGER_GPS_DISTANCE_TO;
    ESP_LOGW(TAG, "Unknown trigger type string: '%s', defaulting to gpio_read", s);
    return RULE_TRIGGER_GPIO_READ;
}

static rule_condition_op_t parse_condition_op(const char *s)
{
    if (strcmp(s, "==") == 0) return RULE_OP_EQ;
    if (strcmp(s, "!=") == 0) return RULE_OP_NE;
    if (strcmp(s, ">") == 0) return RULE_OP_GT;
    if (strcmp(s, "<") == 0) return RULE_OP_LT;
    if (strcmp(s, ">=") == 0) return RULE_OP_GE;
    if (strcmp(s, "<=") == 0) return RULE_OP_LE;
    if (strcmp(s, "any_high") == 0) return RULE_OP_ANY_HIGH;
    if (strcmp(s, "all_low") == 0) return RULE_OP_ALL_LOW;
    if (strcmp(s, "mod_eq") == 0) return RULE_OP_MOD_EQ;
    if (strcmp(s, "mod_ne") == 0) return RULE_OP_MOD_NE;
    return RULE_OP_EQ;
}

static rule_action_type_t parse_action_type(const char *s)
{
    if (strcmp(s, "gpio_write") == 0) return RULE_ACTION_GPIO_WRITE;
    if (strcmp(s, "pwm_set") == 0) return RULE_ACTION_PWM_SET;
    if (strcmp(s, "pwm_release") == 0) return RULE_ACTION_PWM_RELEASE;
    if (strcmp(s, "script_run") == 0) return RULE_ACTION_SCRIPT_RUN;
    if (strcmp(s, "escalate") == 0) return RULE_ACTION_ESCALATE;
    return RULE_ACTION_GPIO_WRITE;
}

static esp_err_t rule_load(void)
{
    FILE *f = fopen(MIMI_RULE_FILE, "r");
    if (!f) {
        ESP_LOGI(TAG, "No rule file found, starting fresh");
        s_rule_count = 0;
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 16384) {
        ESP_LOGW(TAG, "Rule file invalid size: %ld", fsize);
        fclose(f);
        s_rule_count = 0;
        return ESP_OK;
    }

    char *buf = malloc(fsize + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    fread(buf, 1, fsize, f);
    buf[fsize] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse rule JSON");
        s_rule_count = 0;
        return ESP_OK;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "rules");
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        s_rule_count = 0;
        return ESP_OK;
    }

    s_rule_count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (s_rule_count >= MAX_RULES) break;

        rule_t *r = &s_rules[s_rule_count];
        memset(r, 0, sizeof(rule_t));

        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
        if (!id || !name) continue;

        strncpy(r->id, id, sizeof(r->id) - 1);
        strncpy(r->name, name, sizeof(r->name) - 1);

        cJSON *enabled_j = cJSON_GetObjectItem(item, "enabled");
        r->enabled = enabled_j ? cJSON_IsTrue(enabled_j) : true;

        cJSON *v;
        v = cJSON_GetObjectItem(item, "interval_s");
        r->interval_s = (v && cJSON_IsNumber(v)) ? (uint32_t)v->valuedouble : 5;

        v = cJSON_GetObjectItem(item, "cooldown_s");
        r->cooldown_s = (v && cJSON_IsNumber(v)) ? (uint32_t)v->valuedouble : 0;

        v = cJSON_GetObjectItem(item, "last_eval");
        r->last_eval = (v && cJSON_IsNumber(v)) ? (int64_t)v->valuedouble : 0;

        v = cJSON_GetObjectItem(item, "last_fire");
        r->last_fire = (v && cJSON_IsNumber(v)) ? (int64_t)v->valuedouble : 0;

        v = cJSON_GetObjectItem(item, "fire_count");
        r->fire_count = (v && cJSON_IsNumber(v)) ? v->valueint : 0;

        v = cJSON_GetObjectItem(item, "counter");
        r->counter = (v && cJSON_IsNumber(v)) ? v->valueint : 0;

        /* Trigger */
        cJSON *trigger_j = cJSON_GetObjectItem(item, "trigger");
        if (trigger_j && cJSON_IsObject(trigger_j)) {
            const char *tt = cJSON_GetStringValue(cJSON_GetObjectItem(trigger_j, "type"));
            if (tt) r->trigger.type = parse_trigger_type(tt);
            if (r->trigger.type != RULE_TRIGGER_INTERVAL) {
                cJSON *pin_j = cJSON_GetObjectItem(trigger_j, "pin");
                if (pin_j && cJSON_IsNumber(pin_j)) r->trigger.pin = pin_j->valueint;
            }
            cJSON *ch_j = cJSON_GetObjectItem(trigger_j, "channel");
            if (ch_j && cJSON_IsNumber(ch_j)) r->trigger.channel = ch_j->valueint;
            cJSON *lat_j = cJSON_GetObjectItem(trigger_j, "lat");
            if (lat_j && cJSON_IsNumber(lat_j)) r->trigger.lat = lat_j->valuedouble;
            cJSON *lon_j = cJSON_GetObjectItem(trigger_j, "lon");
            if (lon_j && cJSON_IsNumber(lon_j)) r->trigger.lon = lon_j->valuedouble;
        }

        /* Condition */
        cJSON *cond_j = cJSON_GetObjectItem(item, "condition");
        if (cond_j && cJSON_IsObject(cond_j)) {
            const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(cond_j, "op"));
            if (op) r->condition.op = parse_condition_op(op);
            cJSON *val_j = cJSON_GetObjectItem(cond_j, "value");
            if (val_j && cJSON_IsNumber(val_j)) r->condition.value = val_j->valueint;
        }

        /* Actions */
        cJSON *acts_j = cJSON_GetObjectItem(item, "actions");
        if (acts_j && cJSON_IsArray(acts_j)) {
            int n = cJSON_GetArraySize(acts_j);
            if (n > RULE_MAX_ACTIONS) n = RULE_MAX_ACTIONS;
            for (int i = 0; i < n; i++) {
                cJSON *a = cJSON_GetArrayItem(acts_j, i);
                if (!a) continue;
                const char *at = cJSON_GetStringValue(cJSON_GetObjectItem(a, "type"));
                if (!at) continue;
                r->actions[i].type = parse_action_type(at);
                cJSON *pin_j = cJSON_GetObjectItem(a, "pin");
                if (pin_j && cJSON_IsNumber(pin_j)) r->actions[i].pin = pin_j->valueint;
                cJSON *val_j = cJSON_GetObjectItem(a, "value");
                if (val_j && cJSON_IsNumber(val_j)) r->actions[i].value = val_j->valueint;
                const char *sn = cJSON_GetStringValue(cJSON_GetObjectItem(a, "script_name"));
                if (sn) strncpy(r->actions[i].script_name, sn, sizeof(r->actions[i].script_name) - 1);
                const char *em = cJSON_GetStringValue(cJSON_GetObjectItem(a, "escalate_msg"));
                if (em) strncpy(r->actions[i].escalate_msg, em, sizeof(r->actions[i].escalate_msg) - 1);
            }
            r->actions_count = n;
        }

        /* Else actions */
        cJSON *else_j = cJSON_GetObjectItem(item, "else_actions");
        if (else_j && cJSON_IsArray(else_j)) {
            int n = cJSON_GetArraySize(else_j);
            if (n > RULE_MAX_ACTIONS) n = RULE_MAX_ACTIONS;
            for (int i = 0; i < n; i++) {
                cJSON *a = cJSON_GetArrayItem(else_j, i);
                if (!a) continue;
                const char *at = cJSON_GetStringValue(cJSON_GetObjectItem(a, "type"));
                if (!at) continue;
                r->else_actions[i].type = parse_action_type(at);
                cJSON *pin_j = cJSON_GetObjectItem(a, "pin");
                if (pin_j && cJSON_IsNumber(pin_j)) r->else_actions[i].pin = pin_j->valueint;
                cJSON *val_j = cJSON_GetObjectItem(a, "value");
                if (val_j && cJSON_IsNumber(val_j)) r->else_actions[i].value = val_j->valueint;
                const char *sn = cJSON_GetStringValue(cJSON_GetObjectItem(a, "script_name"));
                if (sn) strncpy(r->else_actions[i].script_name, sn, sizeof(r->else_actions[i].script_name) - 1);
                const char *em = cJSON_GetStringValue(cJSON_GetObjectItem(a, "escalate_msg"));
                if (em) strncpy(r->else_actions[i].escalate_msg, em, sizeof(r->else_actions[i].escalate_msg) - 1);
            }
            r->else_actions_count = n;
        }

        s_rule_count++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d rules", s_rule_count);
    return ESP_OK;
}

esp_err_t rule_engine_save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_rule_count; i++) {
        rule_t *r = &s_rules[i];
        cJSON *item = cJSON_CreateObject();

        cJSON_AddStringToObject(item, "id", r->id);
        cJSON_AddStringToObject(item, "name", r->name);
        cJSON_AddBoolToObject(item, "enabled", r->enabled);
        cJSON_AddNumberToObject(item, "interval_s", r->interval_s);
        cJSON_AddNumberToObject(item, "cooldown_s", r->cooldown_s);
        cJSON_AddNumberToObject(item, "last_eval", (double)r->last_eval);
        cJSON_AddNumberToObject(item, "last_fire", (double)r->last_fire);
        cJSON_AddNumberToObject(item, "fire_count", r->fire_count);
        cJSON_AddNumberToObject(item, "counter", r->counter);

        /* Trigger */
        cJSON *trigger_j = cJSON_CreateObject();
        cJSON_AddStringToObject(trigger_j, "type", trigger_type_str(r->trigger.type));
        if (r->trigger.type == RULE_TRIGGER_GPIO_READ) {
            cJSON_AddNumberToObject(trigger_j, "pin", r->trigger.pin);
        }
        if (r->trigger.type == RULE_TRIGGER_ULTRASONIC_DISTANCE ||
            r->trigger.type == RULE_TRIGGER_IMU_TILT) {
            cJSON_AddNumberToObject(trigger_j, "channel", r->trigger.channel);
        }
        if (r->trigger.type == RULE_TRIGGER_GPS_DISTANCE_TO) {
            cJSON_AddNumberToObject(trigger_j, "lat", r->trigger.lat);
            cJSON_AddNumberToObject(trigger_j, "lon", r->trigger.lon);
        }
        cJSON_AddItemToObject(item, "trigger", trigger_j);

        /* Condition */
        cJSON *cond_j = cJSON_CreateObject();
        cJSON_AddStringToObject(cond_j, "op", op_str(r->condition.op));
        cJSON_AddNumberToObject(cond_j, "value", r->condition.value);
        cJSON_AddItemToObject(item, "condition", cond_j);

        /* Actions helper */
        cJSON *actions = cJSON_CreateArray();
        for (int j = 0; j < r->actions_count; j++) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "type", action_type_str(r->actions[j].type));
            cJSON_AddNumberToObject(entry, "pin", r->actions[j].pin);
            cJSON_AddNumberToObject(entry, "value", r->actions[j].value);
            if (r->actions[j].script_name[0]) cJSON_AddStringToObject(entry, "script_name", r->actions[j].script_name);
            if (r->actions[j].escalate_msg[0]) cJSON_AddStringToObject(entry, "escalate_msg", r->actions[j].escalate_msg);
            cJSON_AddItemToArray(actions, entry);
        }
        cJSON_AddItemToObject(item, "actions", actions);

        cJSON *else_actions = cJSON_CreateArray();
        for (int j = 0; j < r->else_actions_count; j++) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "type", action_type_str(r->else_actions[j].type));
            cJSON_AddNumberToObject(entry, "pin", r->else_actions[j].pin);
            cJSON_AddNumberToObject(entry, "value", r->else_actions[j].value);
            if (r->else_actions[j].script_name[0]) cJSON_AddStringToObject(entry, "script_name", r->else_actions[j].script_name);
            if (r->else_actions[j].escalate_msg[0]) cJSON_AddStringToObject(entry, "escalate_msg", r->else_actions[j].escalate_msg);
            cJSON_AddItemToArray(else_actions, entry);
        }
        cJSON_AddItemToObject(item, "else_actions", else_actions);

        cJSON_AddItemToArray(arr, item);
    }

    cJSON_AddItemToObject(root, "rules", arr);

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) return ESP_ERR_NO_MEM;

    FILE *f = fopen(MIMI_RULE_FILE, "w");
    if (!f) {
        free(json_str);
        return ESP_FAIL;
    }
    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, f);
    fclose(f);
    free(json_str);

    if (written != len) {
        ESP_LOGE(TAG, "Save incomplete: %d/%d bytes", (int)written, (int)len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved %d rules to %s", s_rule_count, MIMI_RULE_FILE);
    return ESP_OK;
}

/* ── Public API ───────────────────────────────────────────────── */

esp_err_t rule_engine_init(void)
{
    s_rule_count = 0;
    s_running = false;
    return rule_load();
}

esp_err_t rule_engine_start(void)
{
    if (s_rule_task) {
        ESP_LOGW(TAG, "Rule engine already running");
        return ESP_OK;
    }

    s_running = true;
    BaseType_t ok = xTaskCreate(
        rule_engine_task,
        "rule_engine",
        MIMI_RULE_STACK,
        NULL,
        MIMI_RULE_PRIO,
        &s_rule_task
    );

    if (ok != pdPASS || !s_rule_task) {
        s_running = false;
        ESP_LOGE(TAG, "Failed to create rule engine task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Rule engine started (%d rules, stack=%d, prio=%d)",
             s_rule_count, MIMI_RULE_STACK, MIMI_RULE_PRIO);
    return ESP_OK;
}

void rule_engine_stop(void)
{
    s_running = false;
    ESP_LOGI(TAG, "Rule engine stop requested");
}

esp_err_t rule_engine_add(const rule_t *rule)
{
    if (s_rule_count >= MAX_RULES) {
        ESP_LOGW(TAG, "Max rules reached (%d)", MAX_RULES);
        return ESP_ERR_NO_MEM;
    }

    rule_t copy = *rule;
    if (copy.id[0] == '\0') {
        generate_id(copy.id);
    }
    copy.enabled = true;
    copy.last_eval = 0;
    copy.last_fire = 0;
    copy.fire_count = 0;

    s_rules[s_rule_count] = copy;
    s_rule_count++;

    rule_engine_save();
    ESP_LOGI(TAG, "Added rule '%s' (%s)", copy.name, copy.id);
    return ESP_OK;
}

esp_err_t rule_engine_remove(const char *rule_id)
{
    for (int i = 0; i < s_rule_count; i++) {
        if (strcmp(s_rules[i].id, rule_id) == 0) {
            ESP_LOGI(TAG, "Removing rule '%s' (%s)", s_rules[i].name, rule_id);
            for (int j = i; j < s_rule_count - 1; j++) {
                s_rules[j] = s_rules[j + 1];
            }
            s_rule_count--;
            rule_engine_save();
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t rule_engine_enable(const char *rule_id)
{
    for (int i = 0; i < s_rule_count; i++) {
        if (strcmp(s_rules[i].id, rule_id) == 0) {
            s_rules[i].enabled = true;
            rule_engine_save();
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t rule_engine_disable(const char *rule_id)
{
    for (int i = 0; i < s_rule_count; i++) {
        if (strcmp(s_rules[i].id, rule_id) == 0) {
            s_rules[i].enabled = false;
            rule_engine_save();
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void rule_engine_list(const rule_t **rules, int *count)
{
    *rules = s_rules;
    *count = s_rule_count;
}
