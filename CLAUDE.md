# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

ESP-IDF v5.5.2 is installed at `~/.espressif/esp-idf-v5.5.2`; `idf.py` is not in PATH. Use the absolute path in scripts:

```bash
/Users/yinbo/.espressif/python_env/idf5.5_py3.9_env/bin/python \
  /Users/yinbo/.espressif/esp-idf-v5.5.2/tools/idf.py <command>
```

For interactive use, `get_idf` alias sources the export script.

```bash
# Full rebuild (required after mimi_secrets.h or sdkconfig changes)
idf.py fullclean && idf.py build

# Flash + monitor (USB JTAG port — see Ports below)
idf.py -p /dev/cu.usbmodem21201 flash monitor

# Pre-flash SPIFFS image (files under spiffs_data/ bundled at build time)
# Happens automatically during build via spiffs_create_partition_image()
```

## Hardware Ports

ESP32-S3 boards have **two USB-C ports**. Using the wrong port is the most common source of flash/monitor failures:

| Port | Label | macOS device | Purpose |
|------|-------|-------------|---------|
| USB | JTAG | `/dev/cu.usbmodem*` | `idf.py flash`, debug |
| COM | UART | `/dev/cu.usbserial-*` | Serial REPL CLI (`idf.py monitor` at 115200) |

The REPL requires the UART port because `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`. The USB port does not support interactive input.

## High-Level Architecture

### Dual-Core Task Layout

| Task | Core | Priority | Description |
|------|------|----------|-------------|
| `tg_poll` / `feishu_webhook` | 0 | 5 | Channel inbound I/O |
| `agent_loop` | 1 | 6 | Message processing + LLM HTTPS call |
| `outbound` | 0 | 5 | Route responses to Telegram / WebSocket |
| `serial_cli` | 0 | 3 | REPL console |
| `nav_l1_reflex` | 0 | 5 | 50 Hz sensor fusion → situation |
| `nav_l2_fsm` | 0 | 5 | 20 Hz navigation state machine |
| `rule_engine` | 0 | 4 | Autonomous sensor-driven rules |
| `cron_service` | 0 | 3 | Scheduled job dispatcher |
| `heartbeat` | 0 | 3 | Periodic HEARTBEAT.md checker |
| httpd | 0 | 5 | WebSocket server (:18789) |

Core 0 handles all I/O. Core 1 is dedicated to the agent loop (CPU-bound JSON building + HTTPS blocking).

### Message Bus

Two FreeRTOS queues carrying `mimi_msg_t` (defined in `bus/message_bus.h`):

- **Inbound queue** (depth 8): channels → agent loop
- **Outbound queue** (depth 8): agent loop → dispatch → channels

Content string ownership is transferred on push; receiver must `free()`.

### ReAct Agent Loop

`agent/agent_loop.c` runs the core AI loop on Core 1:

1. Pop message from inbound queue
2. Load session history (`memory/session_mgr.c` reads JSONL from SPIFFS)
3. Build system prompt (`agent/context_builder.c` assembles SOUL.md + USER.md + MEMORY.md + recent notes + tool schemas)
4. ReAct loop (max 10 iterations):
   - Call LLM via `llm_proxy.c` (Anthropic Messages API non-streaming, or OpenAI)
   - Parse response → text blocks + `tool_use` blocks
   - If `stop_reason == "tool_use"`: execute tools, append results, repeat
   - If `stop_reason == "end_turn"`: break with final text
5. Save user message + assistant reply to session JSONL
6. Push response to outbound queue

Large buffers (32 KB+) are allocated from PSRAM via `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)`.

### Tool Registry Pattern

Tools are registered at runtime in `tool_registry_init()` (`tools/tool_registry.c`). Each tool is a `mimi_tool_t` struct with:

- `.name` — tool identifier (must match LLM tool_use name exactly)
- `.description` — passed to LLM in tools array
- `.input_schema_json` — JSON Schema for the LLM
- `.execute` — C function receiving JSON string, writing output to a buffer

Tools live in `tools/tool_*.c`. To add a new tool: define the execute function, register it in `tool_registry_init()`, and rebuild.

### Navigation Stack (RC Car Autopilot)

The nav subsystem is a layered autonomy stack driving an RC car with GPS, IMU, and 3× ultrasonic sensors.

```
Drivers (FreeRTOS tasks)
├── driver_ultrasonic.c  → 3× HC-SR04 (left/front/right)
├── driver_imu.c         → MPU6050 (roll/pitch/yaw via complementary filter)
├── driver_gps.c         → NEO-6M NMEA parser → nav_gps_filter
│
nav_situation.c          ← Single source of truth (mutex-protected struct)
│   ├── distances_cm[3]  ← ultrasonic
│   ├── yaw_deg          ← IMU
│   ├── lat/lon/fix      ← GPS filter output
│   └── goal_lat/lon     ← set by nav_controller
│
nav_planner.c            → Bearing/distance to goal, heading error
│
nav_controller.c         → Orchestrates L1 + L2
│   ├── nav_l1_reflex.c  → 50 Hz: read sensors → update situation
│   └── nav_l2_fsm.c     → 20 Hz: state machine → output throttle/steer
│       States: CRUISE → AVOID_LEFT/RIGHT → REVERSE → REPLAN → FAULT
│
tools/tool_pwm.c         → PWM output to servo (GPIO4) + ESC (GPIO5)
```

**Critical initialization order** (`tool_registry.c`): `tool_nav_init()` (creates `nav_gps_filter` mutex) **must** run before `tool_sensors_init()` (starts `driver_gps_task` which calls `nav_gps_filter_update()`). The GPS task can fire NMEA callbacks immediately after `driver_gps_start()`.

**L2 FSM** (`nav_l2_fsm.c`) runs at 50 ms period. On obstacle detection (`front < avoid_trigger_cm`), `score_sides()` evaluates left vs right using:
1. **Clearance** — ultrasonic left/right distance
2. **Heading benefit** — how close the turned heading is to goal bearing
3. **Memory penalty** — local failure history from `nav_memory.c` (8-record ring buffer, 10 m radius)

### SPIFFS Storage Model

SPIFFS is mounted at `/spiffs` (12 MB partition). It is a flat filesystem — paths are just filenames.

| Path | Purpose |
|------|---------|
| `/spiffs/config/SOUL.md` | AI personality |
| `/spiffs/config/USER.md` | User profile |
| `/spiffs/config/rc.json` | RC car servo/ESC calibration |
| `/spiffs/config/sensors.json` | Sensor pin assignments |
| `/spiffs/memory/MEMORY.md` | Long-term memory |
| `/spiffs/memory/YYYY-MM-DD.md` | Daily notes |
| `/spiffs/sessions/tg_<id>.jsonl` | Chat history (one JSON object per line) |
| `/spiffs/skills/*.md` | Custom skill prompts |
| `/spiffs/cron.json` | Persisted cron jobs |
| `/spiffs/rules.json` | Persisted autonomous rules |
| `/spiffs/logs/run_XXXX.log` | Offline runtime logs |

Files under `spiffs_data/` at build time are pre-flashed into the SPIFFS image.

### Configuration System (Three Layers)

1. **Build-time defaults** — `mimi_config.h` + `mimi_secrets.h` (highest priority)
2. **Runtime overrides** — NVS flash (set via CLI commands like `wifi_set`, `set_api_key`, `set_model`)
3. **SPIFFS JSON configs** — `sensors.json`, `rc.json`, `rules.json`, `cron.json`

NVS values take priority over build-time secrets for WiFi, Telegram token, API key, model, proxy. This allows field reconfiguration without recompiling.

### GPIO Policy

`tools/gpio_policy.h` defines an allowlist of safe GPIOs. Any tool or driver requesting a disallowed pin is rejected at init time. This prevents collisions with system pins (USB, Flash/PSRAM, BOOT).

### Offline Logging

All runtime logs are mirrored to SPIFFS (`/spiffs/logs/run_XXXX.log`). Logs rotate by boot count, max 512 KB per file. CLI commands: `log_list`, `log_read`, `log_delete`, `log_clear`, `log_status`.

### Flash Partitions

| Offset | Size | Name | Purpose |
|--------|------|------|---------|
| 0x009000 | 24 KB | nvs | WiFi calibration |
| 0x00F000 | 8 KB | otadata | OTA boot state |
| 0x011000 | 4 KB | phy_init | WiFi PHY calibration |
| 0x020000 | 2 MB | ota_0 | Firmware slot A |
| 0x220000 | 2 MB | ota_1 | Firmware slot B |
| 0x420000 | 12 MB | spiffs | Filesystem |
| 0xFF0000 | 64 KB | coredump | Crash dump storage |

## Key Files for Common Changes

| Change | File(s) |
|--------|---------|
| Add a new tool | `tools/tool_*.c` + register in `tools/tool_registry.c` |
| Change AI model / API key | `main/mimi_secrets.h` (build-time) or CLI `set_model` / `set_api_key` (runtime) |
| Change WiFi credentials | `main/mimi_secrets.h` or CLI `wifi_set` |
| Change RC servo/ESC pins | `spiffs_data/config/rc.json` |
| Change sensor pins | `spiffs_data/config/sensors.json` |
| Change navigation parameters | `nav/nav_config.c` (compile-time defaults) |
| Add a new channel | Implement send/recv in `channels/`, register in `mimi.c` + `message_bus.c` |
| Change personality | `spiffs_data/config/SOUL.md` |
| Pre-flash files | Add to `spiffs_data/`, rebuild |

## Important Non-Obvious Constraints

- `mimi_secrets.h` is gitignored. Copy from `mimi_secrets.h.example` on first clone.
- After any `mimi_secrets.h` or `sdkconfig` change, run `idf.py fullclean && idf.py build`.
- The navigation stack uses **ENU coordinates** internally (origin set on first valid GPS fix). GPS coordinates are converted to local meters for Kalman filtering, then back to lat/lon for output.
- `nav_gps_filter.c` maintains a 4-state Kalman filter (px, py, vx, vy) with satellite-count-based observation noise scaling.
- Magnetometer (HMC5883L) provides absolute yaw via complementary filter with gyro, replacing the old GPS COG bootstrap method.
- The `llm_proxy.c` uses non-streaming Anthropic Messages API. Each agent loop iteration allocates ~32 KB from PSRAM for the JSON buffer.
- WebSocket server runs on port 18789, max 4 clients.
- Feishu webhook runs on port 18790 (separate from WebSocket).
- **Module initialization order matters**. `tool_nav_init()` must run before `tool_sensors_init()` because the latter starts `driver_gps_task`, which immediately calls `nav_gps_filter_update()` upon receiving NMEA data. If the filter mutex is not initialized first, `xSemaphoreTake(NULL)` triggers a FreeRTOS assert panic. Always ensure resources are initialized before tasks that use them are started.
