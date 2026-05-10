# Phase 8: 规则引擎导航扩展 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为规则引擎新增 3 种传感器触发类型（超声波距离、IMU 倾角、GPS 距离），使 LLM 能创建如 "前方 < 30cm 时 escalate" 的自动规则，并更新 progress.md + 提交 git。

**Architecture:** 规则引擎任务已在 1 秒轮询循环中运行；新 eval 函数直接调用 `nav_situation_get()` 读取传感器快照，触发值（整数：cm / 度 / 米）传给已有的 `check_condition()` 比较，无需修改条件层。

**Tech Stack:** C (ESP-IDF v5.5.2), FreeRTOS, cJSON, `nav_situation_t`, `nav_planner_distance_m()`

---

## 文件改动清单

| 文件 | 操作 | 改动内容 |
|------|------|---------|
| `main/rule_engine/rule_engine.h` | 修改 | 新增 3 个 enum 值，扩展 `rule_trigger_t` 结构体 |
| `main/rule_engine/rule_engine.c` | 修改 | 新增 3 个 eval 函数，更新 dispatch/parse/load/save |
| `main/tools/tool_rule.c` | 修改 | 更新 parse_trigger_type，更新 rule_add 解析与验证，更新 rule_list 显示 |
| `main/tools/tool_registry.c` | 修改 | 更新 rule_add 工具 schema |
| `progress.md` | 修改 | 新增 Phase 8 完成记录 |

---

### Task 1: 扩展 `rule_engine.h`

**Files:**
- Modify: `main/rule_engine/rule_engine.h`

- [ ] **Step 1: 在 enum 末尾追加 3 个新触发类型**

将 `rule_engine.h` 中的 `rule_trigger_type_t` 从：
```c
typedef enum {
    RULE_TRIGGER_GPIO_READ = 0,
    RULE_TRIGGER_GPIO_READ_ALL,
    RULE_TRIGGER_INTERVAL,  /* time-based unconditional trigger */
} rule_trigger_type_t;
```
改为：
```c
typedef enum {
    RULE_TRIGGER_GPIO_READ = 0,
    RULE_TRIGGER_GPIO_READ_ALL,
    RULE_TRIGGER_INTERVAL,           /* time-based unconditional trigger */
    RULE_TRIGGER_ULTRASONIC_DISTANCE, /* nav_situation distances_cm[channel] */
    RULE_TRIGGER_IMU_TILT,            /* |roll| (ch=0) or |pitch| (ch=1) in degrees */
    RULE_TRIGGER_GPS_DISTANCE_TO,     /* haversine(cur_pos, lat, lon) in meters */
} rule_trigger_type_t;
```

- [ ] **Step 2: 扩展 `rule_trigger_t` 结构体**

将：
```c
typedef struct {
    rule_trigger_type_t type;
    int pin;            /* for GPIO_READ */
} rule_trigger_t;
```
改为：
```c
typedef struct {
    rule_trigger_type_t type;
    int pin;            /* for GPIO_READ */
    int channel;        /* ultrasonic: 0=left,1=front,2=right; imu: 0=roll,1=pitch */
    double lat;         /* reference point for GPS_DISTANCE_TO */
    double lon;
} rule_trigger_t;
```

- [ ] **Step 3: 验证编译（只改了头文件，暂时不运行）**

```bash
/Users/yinbo/.espressif/python_env/idf5.5_py3.9_env/bin/python \
  /Users/yinbo/.espressif/esp-idf-v5.5.2/tools/idf.py build 2>&1 | tail -20
```
预期：因为 .c 文件未同步会有编译错误（结构体大小不匹配），但这是预期的，继续下一 task。

---

### Task 2: 扩展 `rule_engine.c` — 新增 eval 函数与 dispatch

**Files:**
- Modify: `main/rule_engine/rule_engine.c`

- [ ] **Step 1: 在文件顶部 include 区域新增 nav 头文件**

在现有 includes（`#include <time.h>` 之后）添加：
```c
#include "nav/nav_situation.h"
#include "nav/nav_planner.h"
#include "esp_timer.h"
#include <math.h>
```

- [ ] **Step 2: 更新 `trigger_type_str` helper 函数**

将：
```c
static const char *trigger_type_str(rule_trigger_type_t t)
{
    switch (t) {
        case RULE_TRIGGER_GPIO_READ: return "gpio_read";
        case RULE_TRIGGER_GPIO_READ_ALL: return "gpio_read_all";
        case RULE_TRIGGER_INTERVAL: return "interval";
        default: return "unknown";
    }
}
```
改为：
```c
static const char *trigger_type_str(rule_trigger_type_t t)
{
    switch (t) {
        case RULE_TRIGGER_GPIO_READ:              return "gpio_read";
        case RULE_TRIGGER_GPIO_READ_ALL:          return "gpio_read_all";
        case RULE_TRIGGER_INTERVAL:               return "interval";
        case RULE_TRIGGER_ULTRASONIC_DISTANCE:    return "ultrasonic_distance";
        case RULE_TRIGGER_IMU_TILT:               return "imu_tilt";
        case RULE_TRIGGER_GPS_DISTANCE_TO:        return "gps_distance_to";
        default: return "unknown";
    }
}
```

- [ ] **Step 3: 在 `eval_trigger_gpio_read_all` 之后插入 3 个新 eval 函数**

在 `/* ── Condition check ─` 注释行前插入：
```c
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
```

- [ ] **Step 4: 更新 `evaluate_rule` 中的 switch dispatch**

在 `evaluate_rule` 的 switch 块中，将：
```c
        default:
            ESP_LOGW(TAG, "Rule '%s': unknown trigger type %d", rule->name, rule->trigger.type);
            return;
```
前面（即 `RULE_TRIGGER_INTERVAL` case 之后）添加：
```c
        case RULE_TRIGGER_ULTRASONIC_DISTANCE:
            trigger_ok = eval_trigger_ultrasonic_distance(rule->trigger.channel, &trigger_value);
            break;
        case RULE_TRIGGER_IMU_TILT:
            trigger_ok = eval_trigger_imu_tilt(rule->trigger.channel, &trigger_value);
            break;
        case RULE_TRIGGER_GPS_DISTANCE_TO:
            trigger_ok = eval_trigger_gps_distance(rule->trigger.lat, rule->trigger.lon, &trigger_value);
            break;
```

- [ ] **Step 5: 更新 `parse_trigger_type`（rule_engine.c 内部版本）**

将 `/* ── Persistence ─` 区域的 `parse_trigger_type` 从：
```c
static rule_trigger_type_t parse_trigger_type(const char *s)
{
    if (strcmp(s, "gpio_read") == 0) return RULE_TRIGGER_GPIO_READ;
    if (strcmp(s, "gpio_read_all") == 0) return RULE_TRIGGER_GPIO_READ_ALL;
    if (strcmp(s, "interval") == 0) return RULE_TRIGGER_INTERVAL;
    return RULE_TRIGGER_GPIO_READ;
}
```
改为：
```c
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
```

- [ ] **Step 6: 更新 `rule_load` — 解析新字段**

在 `rule_load` 的触发器解析块中，在 `if (r->trigger.type != RULE_TRIGGER_INTERVAL)` 块之后添加：
```c
        cJSON *ch_j = cJSON_GetObjectItem(trigger_j, "channel");
        if (ch_j && cJSON_IsNumber(ch_j)) r->trigger.channel = ch_j->valueint;
        cJSON *lat_j = cJSON_GetObjectItem(trigger_j, "lat");
        if (lat_j && cJSON_IsNumber(lat_j)) r->trigger.lat = lat_j->valuedouble;
        cJSON *lon_j = cJSON_GetObjectItem(trigger_j, "lon");
        if (lon_j && cJSON_IsNumber(lon_j)) r->trigger.lon = lon_j->valuedouble;
```

- [ ] **Step 7: 更新 `rule_engine_save` — 序列化新字段**

在 save 函数的触发器序列化块（`cJSON_AddItemToObject(item, "trigger", trigger_j);` 之前）中，在现有 pin 序列化之后添加：
```c
        if (r->trigger.type == RULE_TRIGGER_ULTRASONIC_DISTANCE ||
            r->trigger.type == RULE_TRIGGER_IMU_TILT) {
            cJSON_AddNumberToObject(trigger_j, "channel", r->trigger.channel);
        }
        if (r->trigger.type == RULE_TRIGGER_GPS_DISTANCE_TO) {
            cJSON_AddNumberToObject(trigger_j, "lat", r->trigger.lat);
            cJSON_AddNumberToObject(trigger_j, "lon", r->trigger.lon);
        }
```

---

### Task 3: 更新 `tool_rule.c` — 解析、验证、显示

**Files:**
- Modify: `main/tools/tool_rule.c`

- [ ] **Step 1: 更新 `parse_trigger_type`（tool_rule.c 内的副本）**

将：
```c
static rule_trigger_type_t parse_trigger_type(const char *s)
{
    if (strcmp(s, "gpio_read") == 0) return RULE_TRIGGER_GPIO_READ;
    if (strcmp(s, "gpio_read_all") == 0) return RULE_TRIGGER_GPIO_READ_ALL;
    if (strcmp(s, "interval") == 0) return RULE_TRIGGER_INTERVAL;
    return RULE_TRIGGER_GPIO_READ;
}
```
改为：
```c
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
```

- [ ] **Step 2: 更新 `tool_rule_add_execute` — 触发器解析与验证**

将触发器解析块中的 GPIO pin 检查从：
```c
        if (rule.trigger.type != RULE_TRIGGER_INTERVAL) {
            cJSON *pin_j = cJSON_GetObjectItem(trigger_j, "pin");
            if (!pin_j || !cJSON_IsNumber(pin_j)) {
                snprintf(output, output_size, "Error: 'trigger.pin' required for gpio_read / gpio_read_all");
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }
            rule.trigger.pin = pin_j->valueint;
        }
```
改为：
```c
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

        /* Parse new nav trigger fields */
        cJSON *ch_j = cJSON_GetObjectItem(trigger_j, "channel");
        if (ch_j && cJSON_IsNumber(ch_j)) rule.trigger.channel = ch_j->valueint;
        cJSON *lat_j = cJSON_GetObjectItem(trigger_j, "lat");
        if (lat_j && cJSON_IsNumber(lat_j)) rule.trigger.lat = lat_j->valuedouble;
        cJSON *lon_j = cJSON_GetObjectItem(trigger_j, "lon");
        if (lon_j && cJSON_IsNumber(lon_j)) rule.trigger.lon = lon_j->valuedouble;

        /* Validate nav trigger fields */
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
            if (rule.trigger.lat == 0.0 && rule.trigger.lon == 0.0) {
                snprintf(output, output_size,
                         "Error: 'trigger.lat' and 'trigger.lon' required for gps_distance_to");
                cJSON_Delete(root);
                return ESP_ERR_INVALID_ARG;
            }
        }
```

- [ ] **Step 3: 更新 `tool_rule_list_execute` — 显示新触发类型**

将：
```c
        const char *tt = (r->trigger.type == RULE_TRIGGER_GPIO_READ) ? "gpio_read" : "gpio_read_all";
        off += snprintf(output + off, output_size - off,
            "  %d. [%s] \"%s\" — %s, eval every %lus, cooldown %lus, %s, fired %d times\n"
            "      trigger=%s(%d), condition=%s %d, actions=%d, else=%d\n",
```
改为：
```c
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
        if (r->trigger.type == RULE_TRIGGER_GPIO_READ || r->trigger.type == RULE_TRIGGER_GPIO_READ_ALL) {
            snprintf(trigger_detail, sizeof(trigger_detail), "pin=%d", r->trigger.pin);
        } else if (r->trigger.type == RULE_TRIGGER_ULTRASONIC_DISTANCE || r->trigger.type == RULE_TRIGGER_IMU_TILT) {
            snprintf(trigger_detail, sizeof(trigger_detail), "ch=%d", r->trigger.channel);
        } else if (r->trigger.type == RULE_TRIGGER_GPS_DISTANCE_TO) {
            snprintf(trigger_detail, sizeof(trigger_detail), "lat=%.4f,lon=%.4f", r->trigger.lat, r->trigger.lon);
        } else {
            snprintf(trigger_detail, sizeof(trigger_detail), "-");
        }
        off += snprintf(output + off, output_size - off,
            "  %d. [%s] \"%s\" — %s, eval every %lus, cooldown %lus, %s, fired %d times\n"
            "      trigger=%s(%s), condition=%s %d, actions=%d, else=%d\n",
```
并将 `tt, r->trigger.pin,` 改为 `tt, trigger_detail,`

---

### Task 4: 更新 `tool_registry.c` — rule_add 工具 Schema

**Files:**
- Modify: `main/tools/tool_registry.c`

- [ ] **Step 1: 更新 rule_add 工具的 description**

将 rule_add 的 description 从：
```c
        .description = "Create a persistent rule that continuously monitors sensors and automatically executes actions when conditions are met. "
            "Use this when the user wants automatic responses like 'when motion detected, turn on light' or 'if temperature exceeds 30, start fan'. "
            "Rules run independently without LLM involvement, making them efficient for real-time control loops. "
            "A rule consists of: trigger (read a sensor), condition (compare the value), actions (what to do when true), and optional else_actions (what to do when false).",
```
改为：
```c
        .description = "Create a persistent rule that continuously monitors sensors and automatically executes actions when conditions are met. "
            "Use this when the user wants automatic responses like 'when obstacle < 30cm, escalate' or 'when tilted > 45 degrees, stop'. "
            "Rules run independently without LLM involvement, making them efficient for real-time control loops. "
            "Nav trigger types: ultrasonic_distance (channel: 0=left,1=front,2=right, value in cm), "
            "imu_tilt (channel: 0=|roll|,1=|pitch|, value in degrees), "
            "gps_distance_to (lat+lon required, value in meters). "
            "A rule consists of: trigger, condition (compare numeric value), actions (when true), optional else_actions (when false).",
```

- [ ] **Step 2: 更新 rule_add 工具的 input_schema_json**

将 trigger 属性中的 enum 从：
```
"enum\":[\"gpio_read\",\"gpio_read_all\",\"interval\"]
```
改为：
```
"enum\":[\"gpio_read\",\"gpio_read_all\",\"interval\",\"ultrasonic_distance\",\"imu_tilt\",\"gps_distance_to\"]
```

并在 trigger properties 中追加 channel / lat / lon 字段。

完整的 rule_add input_schema_json 改为：
```c
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short name for the rule\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Seconds between evaluations (e.g. 1 for fast response)\"},"
            "\"cooldown_s\":{\"type\":\"integer\",\"description\":\"Minimum seconds between firings (prevents jitter, default 0)\"},"
            "\"trigger\":{\"type\":\"object\",\"description\":\"Sensor to read\","
              "\"properties\":{"
                "\"type\":{\"type\":\"string\","
                  "\"enum\":[\"gpio_read\",\"gpio_read_all\",\"interval\","
                             "\"ultrasonic_distance\",\"imu_tilt\",\"gps_distance_to\"],"
                  "\"description\":\"gpio_read/gpio_read_all: GPIO; interval: time-based; "
                                   "ultrasonic_distance: HC-SR04 distance in cm; "
                                   "imu_tilt: |roll|(ch=0) or |pitch|(ch=1) in degrees; "
                                   "gps_distance_to: haversine distance in meters\"},"
                "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin (required for gpio_read/gpio_read_all)\"},"
                "\"channel\":{\"type\":\"integer\",\"description\":\"Channel: ultrasonic 0=left,1=front,2=right; imu 0=|roll|,1=|pitch|\"},"
                "\"lat\":{\"type\":\"number\",\"description\":\"Reference latitude (required for gps_distance_to)\"},"
                "\"lon\":{\"type\":\"number\",\"description\":\"Reference longitude (required for gps_distance_to)\"}"
              "},"
              "\"required\":[\"type\"]},"
            "\"condition\":{\"type\":\"object\",\"description\":\"Comparison against integer value\","
              "\"properties\":{"
                "\"op\":{\"type\":\"string\","
                  "\"enum\":[\"==\",\"!=\",\">\",\"<\",\">=\",\"<=\",\"any_high\",\"all_low\",\"mod_eq\",\"mod_ne\"],"
                  "\"description\":\"Comparison operator\"},"
                "\"value\":{\"type\":\"integer\",\"description\":\"Threshold: cm for ultrasonic, degrees for imu_tilt, meters for gps_distance_to\"}"
              "},"
              "\"required\":[\"op\",\"value\"]},"
            "\"actions\":{\"type\":\"array\",\"description\":\"Actions when condition is TRUE (max 4)\","
              "\"items\":{\"type\":\"object\","
                "\"properties\":{"
                  "\"type\":{\"type\":\"string\","
                    "\"enum\":[\"gpio_write\",\"pwm_set\",\"pwm_release\",\"script_run\",\"escalate\"]},"
                  "\"pin\":{\"type\":\"integer\"},"
                  "\"value\":{\"type\":\"integer\"},"
                  "\"script_name\":{\"type\":\"string\"},"
                  "\"escalate_msg\":{\"type\":\"string\"}"
                "},"
                "\"required\":[\"type\"]}},"
            "\"else_actions\":{\"type\":\"array\",\"description\":\"Actions when condition is FALSE (max 4)\","
              "\"items\":{\"type\":\"object\","
                "\"properties\":{"
                  "\"type\":{\"type\":\"string\","
                    "\"enum\":[\"gpio_write\",\"pwm_set\",\"pwm_release\",\"script_run\",\"escalate\"]},"
                  "\"pin\":{\"type\":\"integer\"},"
                  "\"value\":{\"type\":\"integer\"},"
                  "\"script_name\":{\"type\":\"string\"},"
                  "\"escalate_msg\":{\"type\":\"string\"}"
                "},"
                "\"required\":[\"type\"]}}"
            "},"
            "\"required\":[\"name\",\"interval_s\",\"trigger\",\"condition\",\"actions\"]}",
```

---

### Task 5: 编译验证

**Files:** (no file changes, build only)

- [ ] **Step 1: 执行编译**

```bash
/Users/yinbo/.espressif/python_env/idf5.5_py3.9_env/bin/python \
  /Users/yinbo/.espressif/esp-idf-v5.5.2/tools/idf.py build 2>&1 | tail -40
```
预期输出：`Project build complete.`（零警告零错误）

- [ ] **Step 2: 如果有编译错误，按错误信息修复**

常见问题：
- `math.h` 中 `fabsf` 未定义 → 确认 `#include <math.h>` 已添加
- 结构体字段不存在 → 检查 `rule_trigger_t` 的 `channel`/`lat`/`lon` 已在 .h 中声明
- nav_situation.h include 路径问题 → 确认路径是 `"nav/nav_situation.h"`

---

### Task 6: 更新 `progress.md`

**Files:**
- Modify: `progress.md`

- [ ] **Step 1: 在文件末尾追加 Phase 8 完成记录**

追加以下内容（替换文件末尾空行处）：

```markdown
### Phase 8: 规则引擎导航扩展 (已完成) ✅

**完成状态：** 100%
**完成时间：** 2026-05-10

**已实现的功能：**

1. **规则引擎扩展 (`rule_engine/rule_engine.{h,c}`)**
   - 新增 3 种触发类型：`RULE_TRIGGER_ULTRASONIC_DISTANCE` / `RULE_TRIGGER_IMU_TILT` / `RULE_TRIGGER_GPS_DISTANCE_TO`
   - 扩展 `rule_trigger_t`：新增 `channel`（int）、`lat`/`lon`（double）字段
   - 新增 3 个 eval 函数，直接读取 `nav_situation_t` 快照：
     - `eval_trigger_ultrasonic_distance(channel)` — 返回 distances_cm[channel]（cm）；stale > 500ms 无效
     - `eval_trigger_imu_tilt(channel)` — 返回 `|roll|`（ch=0）或 `|pitch|`（ch=1）取整（度）
     - `eval_trigger_gps_distance(lat, lon)` — 返回 haversine 距离（米）；GPS 无 fix 或 stale > 5s 无效
   - 更新 parse/load/save：新字段完整序列化/反序列化到规则 JSON 文件

2. **工具层扩展 (`tools/tool_rule.c` + `tool_registry.c`)**
   - `parse_trigger_type` 识别 3 个新字符串
   - `tool_rule_add_execute`：解析 `channel`/`lat`/`lon`；对各类型做参数验证
   - `tool_rule_list_execute`：按触发类型显示 `ch=N` 或 `lat/lon` 详情
   - `rule_add` LLM schema：enum 枚举 6 种触发类型 + channel/lat/lon 字段描述

**验收示例 — "前方 < 30cm escalate" 规则：**
```json
{
  "name": "close_obstacle",
  "interval_s": 1,
  "cooldown_s": 30,
  "trigger": {"type": "ultrasonic_distance", "channel": 1},
  "condition": {"op": "<", "value": 30},
  "actions": [{"type": "escalate", "escalate_msg": "Rule: Front sensor < 30cm obstacle detected"}]
}
```

**引脚分配（继承前序 Phase，无新增）：**
- 超声波：L(TRIG=10,ECHO=12) F(TRIG=13,ECHO=14) R(TRIG=15,ECHO=16)
- IMU I2C：SDA=8, SCL=9
- GPS UART1：RX=17, TX=18
- 电机 ESC：GPIO 21，舵机：GPIO 11

**产出文件清单：**
- 修改：`main/rule_engine/rule_engine.{h,c}` `main/tools/tool_rule.c` `main/tools/tool_registry.c`
- 修改：`progress.md`
```

---

### Task 7: Git 提交

- [ ] **Step 1: 查看变更文件**

```bash
git status
git diff --stat
```

- [ ] **Step 2: 暂存并提交**

```bash
git add main/rule_engine/rule_engine.h \
        main/rule_engine/rule_engine.c \
        main/tools/tool_rule.c \
        main/tools/tool_registry.c \
        progress.md \
        docs/superpowers/plans/2026-05-10-phase8-rule-engine-nav-extension.md
git commit -m "$(cat <<'EOF'
Phase 8: 规则引擎导航传感器触发扩展

新增 ultrasonic_distance / imu_tilt / gps_distance_to 三种规则触发类型，
规则引擎可直接读取 nav_situation_t 快照评估条件（距离 cm、倾角度、GPS 距米），
无需 LLM 参与即可实现 "前方 < 30cm 上报" 等实时监控规则。

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## 自检清单

### Spec 覆盖率
- [x] `RULE_TRIGGER_ULTRASONIC_DISTANCE` — Task 1-2
- [x] `RULE_TRIGGER_IMU_TILT` — Task 1-2
- [x] `RULE_TRIGGER_GPS_DISTANCE_TO` — Task 1-2
- [x] `rule_trigger_t.channel` / `.lat` / `.lon` — Task 1
- [x] `parse_trigger_type` 加新字符串（rule_engine.c + tool_rule.c） — Task 2+3
- [x] `rule_load` / `rule_engine_save` 新字段 — Task 2
- [x] tool_rule.c 解析+验证 — Task 3
- [x] tool_registry.c schema 扩展 — Task 4
- [x] 验收用例：LLM 创建 "front<30 escalate" 规则 — 可通过 Task 4 schema 执行

### 类型一致性
- `rule_trigger_t.channel` = `int`，在 eval_trigger_ultrasonic_distance 和 eval_trigger_imu_tilt 中用 `rule->trigger.channel` 读取 ✓
- `rule_trigger_t.lat/lon` = `double`，传给 `eval_trigger_gps_distance` 和 `nav_planner_distance_m` ✓
- condition `value` 仍为 `int`，eval 函数均返回 `int *out_value` ✓
- `rule_list_execute` 中 `trigger_detail` 字符串由 switch 分支构建，与新 `tt` switch 匹配 ✓
