#include "tools/tool_rule.h"
#include "rule_engine/rule_engine.h"
#include "mimi_config.h"

#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "tool_rule";

/* ── Parse helpers ─────────────────────────────────────────────── */

static rule_trigger_type_t parse_trigger_type(const char *s)
{
    if (strcmp(s, "gpio_read") == 0) return RULE_TRIGGER_GPIO_READ;
    if (strcmp(s, "gpio_read_all") == 0) return RULE_TRIGGER_GPIO_READ_ALL;
    if (strcmp(s, "interval") == 0) return RULE_TRIGGER_INTERVAL;
    if (strcmp(s, "ultrasonic_distance") == 0) return RULE_TRIGGER_ULTRASONIC_DISTANCE;
    if (strcmp(s, "imu_tilt") == 0) return RULE_TRIGGER_IMU_TILT;
    if (strcmp(s, "gps_distance_to") == 0) return RULE_TRIGGER_GPS_DISTANCE_TO;
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

static bool parse_actions(cJSON *arr, rule_action_entry_t *out, int *out_count)
{
    if (!arr || !cJSON_IsArray(arr)) {
        *out_count = 0;
        return true;
    }

    int n = cJSON_GetArraySize(arr);
    if (n > RULE_MAX_ACTIONS) n = RULE_MAX_ACTIONS;

    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_GetArrayItem(arr, i);
        if (!a) continue;

        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(a, "type"));
        if (!type) continue;

        out[i].type = parse_action_type(type);
        cJSON *pin_j = cJSON_GetObjectItem(a, "pin");
        out[i].pin = (pin_j && cJSON_IsNumber(pin_j)) ? pin_j->valueint : 0;
        cJSON *val_j = cJSON_GetObjectItem(a, "value");
        out[i].value = (val_j && cJSON_IsNumber(val_j)) ? val_j->valueint : 0;
        const char *sn = cJSON_GetStringValue(cJSON_GetObjectItem(a, "script_name"));
        if (sn) {
            strncpy(out[i].script_name, sn, sizeof(out[i].script_name) - 1);
            out[i].script_name[sizeof(out[i].script_name) - 1] = '\0';
        }
        const char *em = cJSON_GetStringValue(cJSON_GetObjectItem(a, "escalate_msg"));
        if (em) {
            strncpy(out[i].escalate_msg, em, sizeof(out[i].escalate_msg) - 1);
            out[i].escalate_msg[sizeof(out[i].escalate_msg) - 1] = '\0';
        }
    }

    *out_count = n;
    return true;
}

/* ── rule_add ──────────────────────────────────────────────────── */

esp_err_t tool_rule_add_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    if (!name || name[0] == '\0') {
        snprintf(output, output_size, "Error: 'name' required (non-empty string)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *interval_j = cJSON_GetObjectItem(root, "interval_s");
    if (!interval_j || !cJSON_IsNumber(interval_j) || interval_j->valuedouble <= 0) {
        snprintf(output, output_size, "Error: 'interval_s' required (positive integer, seconds between evaluations)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    rule_t rule;
    memset(&rule, 0, sizeof(rule));
    strncpy(rule.name, name, sizeof(rule.name) - 1);
    rule.interval_s = (uint32_t)interval_j->valuedouble;

    cJSON *cooldown_j = cJSON_GetObjectItem(root, "cooldown_s");
    if (cooldown_j && cJSON_IsNumber(cooldown_j)) {
        rule.cooldown_s = (uint32_t)cooldown_j->valuedouble;
    }

    /* Trigger */
    cJSON *trigger_j = cJSON_GetObjectItem(root, "trigger");
    if (trigger_j && cJSON_IsObject(trigger_j)) {
        const char *tt = cJSON_GetStringValue(cJSON_GetObjectItem(trigger_j, "type"));
        if (tt) {
            rule.trigger.type = parse_trigger_type(tt);
        } else {
            snprintf(output, output_size, "Error: 'trigger.type' required (gpio_read, gpio_read_all, or interval)");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        if (rule.trigger.type == RULE_TRIGGER_GPIO_READ ||
            rule.trigger.type == RULE_TRIGGER_GPIO_READ_ALL) {
            cJSON *pin_j = cJSON_GetObjectItem(trigger_j, "pin");
            if (!pin_j || !cJSON_IsNumber(pin_j)) {
                snprintf(output, output_size, "Error: 'trigger.pin' required for gpio_read / gpio_read_all");
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }
            rule.trigger.pin = pin_j->valueint;
        }

        /* Parse nav sensor trigger fields */
        cJSON *ch_j = cJSON_GetObjectItem(trigger_j, "channel");
        if (ch_j && cJSON_IsNumber(ch_j)) rule.trigger.channel = ch_j->valueint;
        cJSON *lat_j = cJSON_GetObjectItem(trigger_j, "lat");
        if (lat_j && cJSON_IsNumber(lat_j)) rule.trigger.lat = lat_j->valuedouble;
        cJSON *lon_j = cJSON_GetObjectItem(trigger_j, "lon");
        if (lon_j && cJSON_IsNumber(lon_j)) rule.trigger.lon = lon_j->valuedouble;

        /* Validate nav sensor trigger parameters */
        if (rule.trigger.type == RULE_TRIGGER_ULTRASONIC_DISTANCE) {
            if (rule.trigger.channel < 0 || rule.trigger.channel > 2) {
                snprintf(output, output_size,
                         "Error: 'trigger.channel' must be 0 (left), 1 (front), or 2 (right) for ultrasonic_distance");
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (rule.trigger.type == RULE_TRIGGER_IMU_TILT) {
            if (rule.trigger.channel < 0 || rule.trigger.channel > 1) {
                snprintf(output, output_size,
                         "Error: 'trigger.channel' must be 0 (|roll|) or 1 (|pitch|) for imu_tilt");
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (rule.trigger.type == RULE_TRIGGER_GPS_DISTANCE_TO) {
            if (!lat_j || !cJSON_IsNumber(lat_j) || !lon_j || !cJSON_IsNumber(lon_j)) {
                snprintf(output, output_size,
                         "Error: 'trigger.lat' and 'trigger.lon' required for gps_distance_to");
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }
        }
    } else {
        snprintf(output, output_size, "Error: 'trigger' required object with 'type' (and 'pin' for gpio_read)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Condition */
    cJSON *cond_j = cJSON_GetObjectItem(root, "condition");
    if (cond_j && cJSON_IsObject(cond_j)) {
        const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(cond_j, "op"));
        if (op) rule.condition.op = parse_condition_op(op);
        cJSON *val_j = cJSON_GetObjectItem(cond_j, "value");
        if (val_j && cJSON_IsNumber(val_j)) rule.condition.value = val_j->valueint;
    } else {
        snprintf(output, output_size, "Error: 'condition' required object with 'op' and 'value'");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Actions */
    if (!parse_actions(cJSON_GetObjectItem(root, "actions"), rule.actions, &rule.actions_count)) {
        snprintf(output, output_size, "Error: invalid 'actions' array");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (rule.actions_count == 0) {
        snprintf(output, output_size, "Error: at least one action required");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    parse_actions(cJSON_GetObjectItem(root, "else_actions"), rule.else_actions, &rule.else_actions_count);

    cJSON_Delete(root);

    esp_err_t err = rule_engine_add(&rule);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to add rule (%s)", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size,
             "OK: Added rule '%s' (id=%s). Evaluates every %lus, cooldown %lus.",
             rule.name, rule.id, (unsigned long)rule.interval_s, (unsigned long)rule.cooldown_s);
    ESP_LOGI(TAG, "rule_add: %s", output);
    return ESP_OK;
}

/* ── rule_remove ───────────────────────────────────────────────── */

esp_err_t tool_rule_remove_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *rule_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "rule_id"));
    if (!rule_id || rule_id[0] == '\0') {
        snprintf(output, output_size, "Error: 'rule_id' required");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char id_copy[16] = {0};
    strncpy(id_copy, rule_id, sizeof(id_copy) - 1);
    cJSON_Delete(root);

    esp_err_t err = rule_engine_remove(id_copy);
    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: Removed rule %s", id_copy);
    } else if (err == ESP_ERR_NOT_FOUND) {
        snprintf(output, output_size, "Error: rule '%s' not found", id_copy);
    } else {
        snprintf(output, output_size, "Error: failed to remove rule (%s)", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "rule_remove: %s -> %s", id_copy, esp_err_to_name(err));
    return err;
}

/* ── rule_list ─────────────────────────────────────────────────── */

esp_err_t tool_rule_list_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    const rule_t *rules;
    int count;
    rule_engine_list(&rules, &count);

    if (count == 0) {
        snprintf(output, output_size, "No active rules.");
        return ESP_OK;
    }

    size_t off = 0;
    off += snprintf(output + off, output_size - off, "Rules (%d):\n", count);

    for (int i = 0; i < count && off < output_size - 1; i++) {
        const rule_t *r = &rules[i];
        const char *tt;
        switch (r->trigger.type) {
            case RULE_TRIGGER_GPIO_READ:           tt = "gpio_read"; break;
            case RULE_TRIGGER_GPIO_READ_ALL:       tt = "gpio_read_all"; break;
            case RULE_TRIGGER_INTERVAL:            tt = "interval"; break;
            case RULE_TRIGGER_ULTRASONIC_DISTANCE: tt = "ultrasonic_distance"; break;
            case RULE_TRIGGER_IMU_TILT:            tt = "imu_tilt"; break;
            case RULE_TRIGGER_GPS_DISTANCE_TO:     tt = "gps_distance_to"; break;
            default:                               tt = "unknown"; break;
        }
        char trigger_detail[48];
        if (r->trigger.type == RULE_TRIGGER_GPIO_READ ||
            r->trigger.type == RULE_TRIGGER_GPIO_READ_ALL) {
            snprintf(trigger_detail, sizeof(trigger_detail), "pin=%d", r->trigger.pin);
        } else if (r->trigger.type == RULE_TRIGGER_ULTRASONIC_DISTANCE ||
                   r->trigger.type == RULE_TRIGGER_IMU_TILT) {
            snprintf(trigger_detail, sizeof(trigger_detail), "ch=%d", r->trigger.channel);
        } else if (r->trigger.type == RULE_TRIGGER_GPS_DISTANCE_TO) {
            snprintf(trigger_detail, sizeof(trigger_detail), "lat=%.4f,lon=%.4f",
                     r->trigger.lat, r->trigger.lon);
        } else {
            snprintf(trigger_detail, sizeof(trigger_detail), "-");
        }
        off += snprintf(output + off, output_size - off,
            "  %d. [%s] \"%s\" — %s, eval every %lus, cooldown %lus, %s, fired %d times\n"
            "      trigger=%s(%s), condition=%s %d, actions=%d, else=%d\n",
            i + 1, r->id, r->name,
            r->enabled ? "enabled" : "disabled",
            (unsigned long)r->interval_s, (unsigned long)r->cooldown_s,
            r->last_fire > 0 ? "active" : "never fired",
            r->fire_count,
            tt, trigger_detail,
            (r->condition.op == RULE_OP_EQ) ? "==" :
            (r->condition.op == RULE_OP_NE) ? "!=" :
            (r->condition.op == RULE_OP_GT) ? ">" :
            (r->condition.op == RULE_OP_LT) ? "<" :
            (r->condition.op == RULE_OP_GE) ? ">=" :
            (r->condition.op == RULE_OP_LE) ? "<=" :
            (r->condition.op == RULE_OP_ANY_HIGH) ? "any_high" :
            (r->condition.op == RULE_OP_ALL_LOW)  ? "all_low"  :
            (r->condition.op == RULE_OP_MOD_EQ)   ? "mod_eq"   : "mod_ne",
            r->condition.value,
            r->actions_count, r->else_actions_count);
    }

    ESP_LOGI(TAG, "rule_list: %d rules", count);
    return ESP_OK;
}

/* ── rule_enable / disable ─────────────────────────────────────── */

esp_err_t tool_rule_enable_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *rule_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "rule_id"));
    if (!rule_id || rule_id[0] == '\0') {
        snprintf(output, output_size, "Error: 'rule_id' required");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char id_copy[16] = {0};
    strncpy(id_copy, rule_id, sizeof(id_copy) - 1);
    cJSON_Delete(root);

    esp_err_t err = rule_engine_enable(id_copy);
    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: Enabled rule %s", id_copy);
    } else if (err == ESP_ERR_NOT_FOUND) {
        snprintf(output, output_size, "Error: rule '%s' not found", id_copy);
    } else {
        snprintf(output, output_size, "Error: failed to enable rule (%s)", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "rule_enable: %s -> %s", id_copy, esp_err_to_name(err));
    return err;
}

esp_err_t tool_rule_disable_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *rule_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "rule_id"));
    if (!rule_id || rule_id[0] == '\0') {
        snprintf(output, output_size, "Error: 'rule_id' required");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char id_copy[16] = {0};
    strncpy(id_copy, rule_id, sizeof(id_copy) - 1);
    cJSON_Delete(root);

    esp_err_t err = rule_engine_disable(id_copy);
    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: Disabled rule %s", id_copy);
    } else if (err == ESP_ERR_NOT_FOUND) {
        snprintf(output, output_size, "Error: rule '%s' not found", id_copy);
    } else {
        snprintf(output, output_size, "Error: failed to disable rule (%s)", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "rule_disable: %s -> %s", id_copy, esp_err_to_name(err));
    return err;
}
