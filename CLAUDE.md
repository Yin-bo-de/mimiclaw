# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Build (ESP-IDF environment must be sourced first)
get_idf && idf.py set-target esp32s3 && idf.py build

# Flash and monitor
idf.py flash monitor

# Build SPIFFS image only
idf.py spiffs_build

# Full clean rebuild
idf.py fullclean && idf.py build
```

- ESP-IDF v5.5.2, target chip: ESP32-S3 with 16MB flash / 8MB octal PSRAM
- Custom partition table at `partitions.csv` (2×2MB OTA slots, ~12MB SPIFFS)
- SPIFFS image is built from `spiffs_data/` and embedded in the firmware binary
- CI builds in Docker container `espressif/idf:v5.5.2`

## Architecture

MimiClaw is an ESP32-S3 firmware that runs a personal AI agent accessible via Telegram, Feishu, WebSocket, and serial CLI. The agent uses a ReAct loop with tool calling, backed by Claude or GPT via HTTP API.

### Startup sequence (`main/mimi.c`)

1. **Core infrastructure**: NVS → event loop → SPIFFS mount
2. **Subsystem init** (non-blocking): display, message bus, memory store, skills, sessions, WiFi manager, HTTP proxy, Telegram, Feishu, LLM proxy, tool registry, cron, heartbeat, agent loop
3. **Serial CLI** starts early (works without WiFi)
4. **WiFi connection** with 30s timeout; falls back to captive portal onboarding on failure
5. **Post-WiFi services**: outbound dispatch task, agent loop, Telegram polling, Feishu webhook, cron, heartbeat, WebSocket server

### Message flow

```
Telegram/Feishu/WebSocket/Serial → Message Bus (inbound queue)
  → Agent Loop (ReAct on Core 1)
    → LLM Proxy (Anthropic/OpenAI API)
    → Tool Registry (web search, cron, GPIO, display, files, time)
  → Message Bus (outbound queue)
    → Outbound Dispatch Task (Core 0) → routes back to originating channel
```

The message bus uses FreeRTOS queues (depth 16). Messages carry a `channel` field (`"telegram"`, `"feishu"`, `"websocket"`, `"system"`) used by the outbound dispatcher to route responses.

### Configuration system

Two layers:
- **Compile-time defaults**: `main/mimi_config.h` — pins, timeouts, stack sizes, API URLs, default model/provider. Secrets are conditionally included from `mimi_secrets.h` (gitignored), with empty-string fallbacks.
- **Runtime overrides**: Serial CLI reads/writes NVS namespaces (`wifi_config`, `tg_config`, `llm_config`, `feishu_config`, `proxy_config`, `search_config`). Settings persist across reboots.

### FreeRTOS task layout (key tasks)

| Task | Core | Stack | Priority |
|------|------|-------|----------|
| Telegram polling | 0 | 12KB | 5 |
| Feishu bot | 0 | 12KB | 4 |
| Outbound dispatch | 0 | 12KB | 4 |
| Agent loop | 1 | 24KB | 6 |
| Display service | 1 | 4KB | 3 |
| Serial CLI | 0 | 4KB | 3 |
| Cron service | timer | — | — |
| Heartbeat | timer | — | — |

### Key modules

- **`agent/`** — ReAct agent loop (`agent_loop.c`) and context builder (`context_builder.c`). The loop: build system prompt → call LLM → parse tool calls → execute tools → feed results back → repeat until final answer or max iterations (10). Supports up to 4 tool calls per turn and 20 messages of history.
- **`channels/`** — Input sources: Telegram bot (long polling), Feishu/Lark bot (webhook on port 18790). Each channel produces messages on the inbound bus.
- **`llm/`** — `llm_proxy.c` handles HTTP requests to Anthropic and OpenAI APIs. Model and provider are runtime-configurable via NVS.
- **`tools/`** — `tool_registry.c` maintains a registry of tools. Individual tools: `tool_cron.c`, `tool_web_search.c`, `tool_get_time.c`, `tool_files.c`, `tool_gpio.c`, `tool_display.c`. GPIO access is gated by `gpio_policy.c`.
- **`memory/`** — `memory_store.c` reads/writes SPIFFS files (`SOUL.md`, `USER.md`, `MEMORY.md`, `HEARTBEAT.md`). `session_mgr.c` manages conversation sessions stored in `/spiffs/sessions/`.
- **`skills/`** — `skill_loader.c` loads skill definitions from `/spiffs/skills/` on SPIFFS.
- **`display/`** — Waveshare 2.9" four-color e-paper (128×296 physical, rotated to 296×128 for landscape). Driver: `epaper_waveshare_2in9_v2.c`, renderer: `display_render.c`, service task: `display_service.c`.
- **`cron/`** — `cron_service.c` schedules recurring jobs (max 16), persisted in `cron.json`.
- **`heartbeat/`** — Periodic health check every 30 minutes.
- **`onboard/`** — WiFi captive portal when STA connection fails. Creates open AP `MimiClaw-XXXX`, serves config page on port 80, reboots after saving.
- **`gateway/`** — WebSocket server on port 18789 (max 4 clients).
- **`proxy/`** — HTTP CONNECT proxy support for outbound API calls.

### Error handling pattern

The codebase uses ESP-IDF's `ESP_RETURN_ON_ERROR` and `ESP_GOTO_ON_ERROR` macros extensively. Functions return `esp_err_t` (ESP_OK on success). These macros log the tag and error before returning/jumping.

### SPIFFS paths

All at `/spiffs/`: `config/SOUL.md`, `config/USER.md`, `memory/MEMORY.md`, `memory/HEARTBEAT.md`, `sessions/`, `skills/`, `cron.json`, `display_state.json`.
