# Phase 5: L1 Reflex Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the 50 Hz L1 emergency-stop reflex layer and nav_controller top-level coordinator, with a dummy-drive CLI test hook so obstacle-stop behavior can be verified (10/10 trials).

**Architecture:** L1 runs as a FreeRTOS task (prio=7, core=1, 20ms tick). It reads `nav_situation_t`, computes min valid distance, and forces throttle=0 via `rc_nav_throttle(0)` whenever `min_d < emergency_stop_cm`. The nav_controller owns task lifecycle and exposes a dummy-drive hook for Phase 5 testing. Both layers are wired into `mimi.c` init sequence.

**Tech Stack:** ESP-IDF v5.5.2, FreeRTOS, LEDC PWM (existing), nav_situation (Phase 4), nav_config (Phase 4)

---

### Task 1: Add internal RC nav API to tool_pwm.{h,c}

**Files:**
- Modify: `main/tools/tool_pwm.h`
- Modify: `main/tools/tool_pwm.c`

- [ ] **Step 1: Add declarations to tool_pwm.h**

Add after `tool_rc_throttle_execute` declaration:

```c
/* Internal API for nav layer — bypass JSON overhead */
esp_err_t rc_nav_throttle(int throttle_pct);  /* -100..100 */
esp_err_t rc_nav_steer(int steer_pct);        /* -100..100 */
```

- [ ] **Step 2: Implement rc_nav_throttle and rc_nav_steer in tool_pwm.c**

Add at end of tool_pwm.c:

```c
esp_err_t rc_nav_throttle(int pct)
{
    if (!s_rc.loaded) return ESP_ERR_INVALID_STATE;
    if (pct < -100) pct = -100;
    if (pct > 100) pct = 100;
    if (s_rc.throttle_reversed) pct = -pct;
    int pulse_us;
    if (pct < 0) {
        pulse_us = s_rc.throttle_neutral_us +
            (int)((int64_t)(s_rc.throttle_reverse_us - s_rc.throttle_neutral_us) * (-pct) / 100);
    } else {
        pulse_us = s_rc.throttle_neutral_us +
            (int)((int64_t)(s_rc.throttle_forward_us - s_rc.throttle_neutral_us) * pct / 100);
    }
    char buf[64];
    return pwm_set_pulse(s_rc.throttle_gpio, pulse_us, buf, sizeof(buf));
}

esp_err_t rc_nav_steer(int pct)
{
    if (!s_rc.loaded) return ESP_ERR_INVALID_STATE;
    if (pct < -100) pct = -100;
    if (pct > 100) pct = 100;
    if (s_rc.steer_reversed) pct = -pct;
    int pulse_us;
    if (pct < 0) {
        pulse_us = s_rc.steer_center_us +
            (int)((int64_t)(s_rc.steer_min_us - s_rc.steer_center_us) * (-pct) / 100);
    } else {
        pulse_us = s_rc.steer_center_us +
            (int)((int64_t)(s_rc.steer_max_us - s_rc.steer_center_us) * pct / 100);
    }
    char buf[64];
    return pwm_set_pulse(s_rc.steer_gpio, pulse_us, buf, sizeof(buf));
}
```

---

### Task 2: Create nav_l1_reflex.{h,c}

**Files:**
- Create: `main/nav/nav_l1_reflex.h`
- Create: `main/nav/nav_l1_reflex.c`

- [ ] **Step 1: Write nav_l1_reflex.h**

```c
#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t nav_l1_init(void);
esp_err_t nav_l1_start(void);

/** True when emergency stop is active (min distance < threshold). */
bool nav_l1_is_blocked(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write nav_l1_reflex.c**

```c
#include "nav/nav_l1_reflex.h"
#include "nav/nav_situation.h"
#include "nav/nav_config.h"
#include "tools/tool_pwm.h"
#include "mimi_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <limits.h>
#include <stdbool.h>

static const char *TAG = "nav_l1";

#define STALE_US  (500 * 1000LL)   /* 500 ms stale threshold */

static volatile bool s_blocked = false;

bool nav_l1_is_blocked(void) { return s_blocked; }

static void l1_task(void *arg)
{
    const nav_config_t *cfg = nav_config_get();
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        nav_situation_t sit;
        nav_situation_get(&sit);

        int64_t now = esp_timer_get_time();
        int min_d = INT_MAX;
        bool any_valid = false;

        for (int i = 0; i < 3; i++) {
            if (sit.distance_valid[i] &&
                (now - sit.distance_ts_us[i]) < STALE_US) {
                if (sit.distances_cm[i] < min_d) {
                    min_d = sit.distances_cm[i];
                }
                any_valid = true;
            }
        }

        if (any_valid && min_d < cfg->emergency_stop_cm) {
            if (!s_blocked) {
                ESP_LOGW(TAG, "EMERGENCY STOP: min_d=%d cm < %d cm",
                         min_d, cfg->emergency_stop_cm);
            }
            s_blocked = true;
            rc_nav_throttle(0);
        } else {
            if (s_blocked) {
                ESP_LOGI(TAG, "Clear — releasing emergency stop (min_d=%d cm)",
                         any_valid ? min_d : -1);
            }
            s_blocked = false;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MIMI_NAV_L1_PERIOD_MS));
    }
}

esp_err_t nav_l1_init(void) { return ESP_OK; }

esp_err_t nav_l1_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
        l1_task, "nav_l1",
        MIMI_NAV_L1_STACK, NULL,
        MIMI_NAV_L1_PRIO, NULL,
        MIMI_NAV_L1_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create L1 task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "L1 reflex task started (prio=%d core=%d period=%dms)",
             MIMI_NAV_L1_PRIO, MIMI_NAV_L1_CORE, MIMI_NAV_L1_PERIOD_MS);
    return ESP_OK;
}
```

---

### Task 3: Create nav_controller.{h,c}

**Files:**
- Create: `main/nav/nav_controller.h`
- Create: `main/nav/nav_controller.c`

- [ ] **Step 1: Write nav_controller.h**

```c
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

esp_err_t nav_controller_init(void);
esp_err_t nav_controller_start(void);

nav_ctrl_state_t nav_controller_get_state(void);

/**
 * Phase 5 test hook: drive forward at fixed speed so L1 can be validated.
 * speed_pct: 1..100 forward speed percentage.
 */
esp_err_t nav_controller_dummy_drive_start(int speed_pct);
void nav_controller_dummy_drive_stop(void);
bool nav_controller_dummy_is_running(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write nav_controller.c**

```c
#include "nav/nav_controller.h"
#include "nav/nav_l1_reflex.h"
#include "tools/tool_pwm.h"
#include "mimi_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "nav_ctrl";

static nav_ctrl_state_t s_state = NAV_CTRL_IDLE;
static volatile bool s_dummy_running = false;
static int s_dummy_speed_pct = 0;

const char *nav_ctrl_state_name(nav_ctrl_state_t s)
{
    switch (s) {
    case NAV_CTRL_IDLE:    return "IDLE";
    case NAV_CTRL_RUNNING: return "RUNNING";
    case NAV_CTRL_PAUSED:  return "PAUSED";
    default:               return "UNKNOWN";
    }
}

nav_ctrl_state_t nav_controller_get_state(void) { return s_state; }
bool nav_controller_dummy_is_running(void) { return s_dummy_running; }

static void dummy_drive_task(void *arg)
{
    ESP_LOGI(TAG, "Dummy drive started at %d%%", s_dummy_speed_pct);
    rc_nav_steer(0);

    while (s_dummy_running) {
        if (!nav_l1_is_blocked()) {
            rc_nav_throttle(s_dummy_speed_pct);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    rc_nav_throttle(0);
    rc_nav_steer(0);
    ESP_LOGI(TAG, "Dummy drive stopped");
    vTaskDelete(NULL);
}

esp_err_t nav_controller_init(void)
{
    esp_err_t err = nav_l1_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nav_l1_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "nav_controller initialized");
    return ESP_OK;
}

esp_err_t nav_controller_start(void)
{
    esp_err_t err = nav_l1_start();
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "nav_controller started (L1 active)");
    return ESP_OK;
}

esp_err_t nav_controller_dummy_drive_start(int speed_pct)
{
    if (speed_pct < 1 || speed_pct > 100) return ESP_ERR_INVALID_ARG;
    if (s_dummy_running) return ESP_ERR_INVALID_STATE;

    s_dummy_speed_pct = speed_pct;
    s_dummy_running = true;
    s_state = NAV_CTRL_RUNNING;

    BaseType_t ok = xTaskCreate(dummy_drive_task, "nav_dummy", 2048, NULL, 4, NULL);
    if (ok != pdPASS) {
        s_dummy_running = false;
        s_state = NAV_CTRL_IDLE;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void nav_controller_dummy_drive_stop(void)
{
    s_dummy_running = false;
    s_state = NAV_CTRL_IDLE;
}
```

---

### Task 4: Update CMakeLists.txt

**Files:**
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Add new source files**

Add after `"nav/nav_planner.c"`:
```
"nav/nav_l1_reflex.c"
"nav/nav_controller.c"
```

---

### Task 5: Wire nav_controller into mimi.c

**Files:**
- Modify: `main/mimi.c`

- [ ] **Step 1: Add include**

Add `#include "nav/nav_controller.h"` near other includes.

- [ ] **Step 2: Add init call**

After `ESP_ERROR_CHECK(tool_registry_init());` in app_main, add:
```c
ESP_ERROR_CHECK(nav_controller_init());
```

- [ ] **Step 3: Add start call**

After `ESP_ERROR_CHECK(ws_server_start());`, add:
```c
ESP_ERROR_CHECK(nav_controller_start());
```

---

### Task 6: Add l1_test CLI command to serial_cli.c

**Files:**
- Modify: `main/cli/serial_cli.c`

- [ ] **Step 1: Add include at top**

Add `#include "nav/nav_controller.h"` near other nav includes.

- [ ] **Step 2: Add args struct and handler function**

Add near the other test command structs/handlers:

```c
/* l1_test */
static struct {
    struct arg_int *speed;
    struct arg_lit *stop;
    struct arg_end *end;
} l1_test_args;

static int cmd_l1_test(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&l1_test_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, l1_test_args.end, argv[0]);
        return 1;
    }

    if (l1_test_args.stop->count > 0) {
        nav_controller_dummy_drive_stop();
        printf("Dummy drive stopped\n");
        return 0;
    }

    int speed = 20;
    if (l1_test_args.speed->count > 0) {
        speed = l1_test_args.speed->ival[0];
    }

    esp_err_t err = nav_controller_dummy_drive_start(speed);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("Dummy drive already running. Use 'l1_test -x' to stop.\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("Failed to start dummy drive: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Dummy drive started at %d%%\n"
           "L1 will stop the car when obstacle < 20cm\n"
           "Use 'l1_test -x' to stop\n", speed);
    return 0;
}
```

- [ ] **Step 3: Register command in serial_cli_init**

After the `gps_test` registration block, add:

```c
/* l1_test */
l1_test_args.speed = arg_int0("s", "speed", "<pct>", "Forward speed %% (default: 20)");
l1_test_args.stop  = arg_lit0("x", "stop",           "Stop dummy drive");
l1_test_args.end   = arg_end(2);
esp_console_cmd_t l1_test_cmd = {
    .command  = "l1_test",
    .help     = "L1 reflex test: drive forward, L1 stops at obstacle. l1_test [-s <pct>] [-x]",
    .func     = &cmd_l1_test,
    .argtable = &l1_test_args,
};
esp_console_cmd_register(&l1_test_cmd);
```

---

### Task 7: Build and fix compile errors

- [ ] **Step 1: Run build**

```bash
/Users/yinbo/.espressif/python_env/idf5.5_py3.9_env/bin/python \
  /Users/yinbo/.espressif/esp-idf-v5.5.2/tools/idf.py build
```

Expected: BUILD SUCCESSFUL

- [ ] **Step 2: Fix any errors**

Common issues:
- Missing `#include "limits.h"` in nav_l1_reflex.c
- `volatile bool` read order warnings
- `pwm_set_pulse` is static — verify new functions are in same .c file

---

### Task 8: Update progress.md and commit

- [ ] **Step 1: Update progress.md** with Phase 5 completion entry.
- [ ] **Step 2: Git commit** with message `feat: Phase 5 - L1 reflex layer + nav_controller`
