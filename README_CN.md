# MimiClaw: $5 芯片上的口袋 AI 助理

<p align="center">
  <img src="assets/banner.png" alt="MimiClaw" width="500" />
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT"></a>
  <a href="https://deepwiki.com/memovai/mimiclaw"><img src="https://img.shields.io/badge/DeepWiki-mimiclaw-blue.svg" alt="DeepWiki"></a>
  <a href="https://discord.gg/r8ZxSvB8Yr"><img src="https://img.shields.io/badge/Discord-mimiclaw-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
  <a href="https://x.com/ssslvky"><img src="https://img.shields.io/badge/X-@ssslvky-black?logo=x" alt="X"></a>
</p>

<p align="center">
  <strong><a href="README.md">English</a> | <a href="README_CN.md">中文</a> | <a href="README_JA.md">日本語</a></strong>
</p>

**$5 芯片上的 AI 助理（OpenClaw）。没有 Linux，没有 Node.js，纯 C。**

MimiClaw 把一块小小的 ESP32-S3 开发板变成你的私人 AI 助理。插上 USB 供电，连上 WiFi，通过 Telegram 跟它对话 — 它能处理你丢给它的任何任务，还会随时间积累本地记忆不断进化 — 全部跑在一颗拇指大小的芯片上。

## 认识 MimiClaw

- **小巧** — 没有 Linux，没有 Node.js，没有臃肿依赖 — 纯 C
- **好用** — 在 Telegram 发消息，剩下的它来搞定
- **忠诚** — 从记忆中学习，跨重启也不会忘
- **能干** — USB 供电，0.5W，24/7 运行
- **可爱** — 一块 ESP32-S3 开发板，$5，没了

## 工作原理

![](assets/mimiclaw.png)

你在 Telegram 发一条消息，ESP32-S3 通过 WiFi 收到后送进 Agent 循环 — LLM 思考、调用工具、读取记忆 — 再把回复发回来。同时支持 **Anthropic (Claude)** 和 **OpenAI (GPT)** 两种提供商，运行时可切换。一切都跑在一颗 $5 的芯片上，所有数据存在本地 Flash。

## 快速开始

### 你需要

- 一块 **ESP32-S3 开发板**，16MB Flash + 8MB PSRAM（如小智 AI 开发板，~¥30）
- 一根 **USB Type-C 数据线**
- 一个 **Telegram Bot Token** — 在 Telegram 找 [@BotFather](https://t.me/BotFather) 创建
- 一个 **Anthropic API Key** — 从 [console.anthropic.com](https://console.anthropic.com) 获取，或一个 **OpenAI API Key** — 从 [platform.openai.com](https://platform.openai.com) 获取

### 安装

```bash
# 需要先安装 ESP-IDF v5.5+:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw

idf.py set-target esp32s3
```

<details>
<summary>Ubuntu 安装</summary>

建议基线：

- Ubuntu 22.04/24.04
- Python >= 3.10
- CMake >= 3.16
- Ninja >= 1.10
- Git >= 2.34
- flex >= 2.6
- bison >= 3.8
- gperf >= 3.1
- dfu-util >= 0.11
- `libusb-1.0-0`、`libffi-dev`、`libssl-dev`

Ubuntu 安装与构建：

```bash
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

./scripts/setup_idf_ubuntu.sh
./scripts/build_ubuntu.sh
```

</details>

<details>
<summary>macOS 安装</summary>

建议基线：

- macOS 12/13/14
- Xcode Command Line Tools
- Homebrew
- Python >= 3.10
- CMake >= 3.16
- Ninja >= 1.10
- Git >= 2.34
- flex >= 2.6
- bison >= 3.8
- gperf >= 3.1
- dfu-util >= 0.11
- `libusb`、`libffi`、`openssl`

macOS 安装与构建：

```bash
xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

./scripts/setup_idf_macos.sh
./scripts/build_macos.sh
```

</details>

### 配置

MimiClaw 使用**两层配置**：`mimi_secrets.h` 提供编译时默认值，串口 CLI 可在运行时覆盖。CLI 设置的值存在 NVS Flash 中，优先级高于编译时值。

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

编辑 `main/mimi_secrets.h`：

```c
#define MIMI_SECRET_WIFI_SSID       "你的WiFi名"
#define MIMI_SECRET_WIFI_PASS       "你的WiFi密码"
#define MIMI_SECRET_TG_TOKEN        "123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11"
#define MIMI_SECRET_API_KEY         "sk-ant-api03-xxxxx"
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"     // "anthropic" 或 "openai"
#define MIMI_SECRET_SEARCH_KEY      ""              // 可选：Brave Search API key
#define MIMI_SECRET_TAVILY_KEY      ""              // 可选：Tavily API key（优先）
#define MIMI_SECRET_PROXY_HOST      ""              // 可选：如 "10.0.0.1"
#define MIMI_SECRET_PROXY_PORT      ""              // 可选：如 "7897"
```

然后编译烧录：

```bash
# 完整编译（修改 mimi_secrets.h 后必须 fullclean）
idf.py fullclean && idf.py build

# 查找串口
ls /dev/cu.usb*          # macOS
ls /dev/ttyACM*          # Linux

# 烧录并监控（将 PORT 替换为你的串口）
# USB 转接器：大概率是 /dev/cu.usbmodem11401（macOS）或 /dev/ttyACM0（Linux）
idf.py -p PORT flash monitor
```

> **注意：请插对 USB 口！** 大多数 ESP32-S3 开发板有两个 Type-C 接口，必须插标有 **USB** 的那个口（原生 USB Serial/JTAG），**不要**插标有 **COM** 的口（外部 UART 桥接）。插错口会导致烧录/监控失败。
>
> <details>
> <summary>查看参考图片</summary>
>
> <img src="assets/esp32s3-usb-port.jpg" alt="请插 USB 口，不要插 COM 口" width="480" />
>
> </details>

### 预烧录 SPIFFS 数据

MimiClaw 支持在首次启动前将文件预烧录到 SPIFFS。将文件放在 `spiffs_data/` 目录下，它们会自动打包进 SPIFFS 镜像并随固件一起烧录。

```
spiffs_data/
├── config/
│   ├── SOUL.md          # AI 人设
│   ├── USER.md          # 用户偏好
│   └── rc.json          # RC 车 / 舵机校准（见下文）
├── memory/
│   └── MEMORY.md        # 长期记忆
└── skills/
    └── *.md             # 自定义技能说明
```

### CLI 命令（通过 UART/COM 口连接）

通过串口连接即可配置和调试。**配置命令**让你无需重新编译就能修改设置 — 随时随地插上 USB 线就能改。

**如何打开 CLI：**

```bash
# 先找到 COM 口
ls /dev/cu.usb*          # macOS — 找 cu.usbserial-*（不是 usbmodem-*）
ls /dev/ttyUSB*          # Linux

# 方式一：idf.py monitor（退出用 Ctrl+]）
idf.py -p /dev/cu.usbserial-XXXX monitor

# 方式二：screen（退出用 Ctrl+A 再按 K）
screen /dev/cu.usbserial-XXXX 115200

# 方式三：minicom
minicom -D /dev/cu.usbserial-XXXX -b 115200
```

> **必须连接 COM（UART）口**，不能用 USB（JTAG）口。连接成功后会看到 `mimi>` 提示符。

**运行时配置**（存入 NVS，覆盖编译时默认值）：

```
mimi> wifi_set MySSID MyPassword   # 换 WiFi
mimi> set_tg_token 123456:ABC...   # 换 Telegram Bot Token
mimi> set_api_key sk-ant-api03-... # 换 API Key（Anthropic 或 OpenAI）
mimi> set_model_provider openai    # 切换提供商（anthropic|openai）
mimi> set_model gpt-4o             # 换模型
mimi> set_proxy 127.0.0.1 7897     # 设置代理
mimi> clear_proxy                  # 清除代理
mimi> set_search_key BSA...        # 设置 Brave Search API Key
mimi> set_tavily_key tvly-...      # 设置 Tavily API Key（优先）
mimi> config_show                  # 查看所有配置（脱敏显示）
mimi> config_reset                 # 清除 NVS，恢复编译时默认值
```

**调试与运维：**

```
mimi> wifi_status              # 连上了吗？
mimi> memory_read              # 看看它记住了什么
mimi> memory_write "内容"       # 写入 MEMORY.md
mimi> heap_info                # 还剩多少内存？
mimi> session_list             # 列出所有会话
mimi> session_clear 12345      # 删除一个会话
mimi> heartbeat_trigger        # 手动触发一次心跳检查
mimi> cron_start               # 立即启动 cron 调度器
mimi> ota_update https://...   # WiFi OTA 固件更新（成功后自动重启）
mimi> restart                  # 重启
```

**离线日志（SPIFFS 本地存储）：**

MimiClaw 每次启动都会自动将所有运行日志写入 SPIFFS，无需连接电脑。回来后插上 USB，通过以下命令查阅：

```
mimi> log_list                   # 列出所有日志文件及大小，* 标注当前活跃文件
mimi> log_status                 # 查看 SPIFFS 空间用量
mimi> log_read run_0003.log      # 将指定日志文件内容打印到串口
mimi> log_delete run_0001.log    # 删除指定日志文件
mimi> log_clear                  # 删除全部日志，立即重新开始记录
```

日志文件保存在 `/spiffs/logs/run_XXXX.log`，按启动次数自动编号。单文件上限 512 KB；SPIFFS 使用率超过 85% 时自动删除最旧的日志。

### USB（JTAG）与 UART：哪个口做什么

大多数 ESP32-S3 开发板有 **两个 USB-C 口**：

| 端口 | 用途 |
|------|------|
| **USB**（JTAG） | `idf.py flash`、JTAG 调试 |
| **COM**（UART） | **REPL 命令行**、串口控制台 |

> **REPL 必须连接 UART（COM）口。** USB（JTAG）口不支持交互式 REPL 输入。

<details>
<summary>端口详情与推荐工作流</summary>

| 端口 | 标注 | 协议 |
|------|------|------|
| **USB** | USB / JTAG | 原生 USB Serial/JTAG |
| **COM** | UART / COM | 外置 UART 桥接芯片（CP2102/CH340） |

ESP-IDF 控制台默认配置为 UART 输出（`CONFIG_ESP_CONSOLE_UART_DEFAULT=y`）。

**同时连接两个口时：**

- USB（JTAG）口负责烧录/下载，并提供辅助串口输出
- UART（COM）口提供主要的交互式控制台，用于 REPL
- macOS 下两个口都会显示为 `/dev/cu.usbmodem*` 或 `/dev/cu.usbserial-*`，用 `ls /dev/cu.usb*` 区分
- Linux 下 USB（JTAG）通常是 `/dev/ttyACM0`，UART 通常是 `/dev/ttyUSB0`

**推荐工作流：**

```bash
# 通过 USB（JTAG）口烧录
idf.py -p /dev/cu.usbmodem11401 flash

# 通过 UART（COM）口打开 REPL
idf.py -p /dev/cu.usbserial-110 monitor
# 或使用任意串口工具：screen、minicom、PuTTY，波特率 115200
```

</details>

## 记忆

MimiClaw 把所有数据存为纯文本文件，可以直接读取和编辑：

| 文件 | 说明 |
|------|------|
| `SOUL.md` | 机器人的人设 — 编辑它来改变行为方式 |
| `USER.md` | 关于你的信息 — 姓名、偏好、语言 |
| `MEMORY.md` | 长期记忆 — 它应该一直记住的事 |
| `HEARTBEAT.md` | 待办清单 — 机器人定期检查并自主执行 |
| `cron.json` | 定时任务 — AI 创建的周期性或一次性任务 |
| `rules.json` | 自主规则 — 无需 LLM 的传感器驱动控制循环 |
| `scripts/` | 命名脚本 — 重复操作的有序工具调用序列 |
| `2026-02-05.md` | 每日笔记 — 今天发生了什么 |
| `tg_12345.jsonl` | 聊天记录 — 你和它的对话 |

## 工具

MimiClaw 同时支持 Anthropic 和 OpenAI 的工具调用 — LLM 在对话中可以调用工具，循环执行直到任务完成（ReAct 模式）。

| 工具 | 说明 |
|------|------|
| `web_search` | 通过 Tavily（优先）或 Brave 搜索网页，获取实时信息 |
| `get_current_time` | 通过 HTTP 获取当前日期和时间，并设置系统时钟 |
| `gpio_write` | 设置 GPIO 引脚为 HIGH 或 LOW，控制 LED、继电器等数字输出 |
| `gpio_read` | 读取单个 GPIO 引脚状态（HIGH/LOW），检测开关、传感器等 |
| `gpio_read_all` | 一次性读取所有允许的 GPIO 引脚状态 |
| `pwm_set` | 在 GPIO 引脚上设置 PWM 脉宽（0-20000 µs），通用于舵机、ESC、LED |
| `pwm_release` | 停止 PWM 输出并释放通道 |
| `rc_steer` | 控制 RC 车转向：-100=左，0=直，+100=右 |
| `rc_throttle` | 控制 RC 车油门：-100=倒退，0=停止，+100=前进 |
| `script_create` | 创建命名脚本（有序工具调用序列），自动化重复操作，无需 LLM 参与 |
| `script_run` | 按名称执行脚本，直接调用工具，零 token 消耗 |
| `script_list` | 列出所有已存脚本及步骤数 |
| `script_remove` | 按名称删除脚本 |
| `cron_add` | 创建定时或一次性任务（LLM 自主创建 cron 任务） |
| `cron_list` | 列出所有已调度的 cron 任务 |
| `cron_remove` | 按 ID 删除 cron 任务 |
| `rule_add` | 创建持久化自主规则，持续监控传感器并在条件满足时自动执行动作 |
| `rule_list` | 列出所有规则及其状态和配置 |
| `rule_remove` | 按 ID 删除规则 |
| `rule_enable` / `rule_disable` | 启用或禁用规则（不删除） |
| `ota_update` | 从 HTTPS URL 下载并烧录固件，成功后自动重启 |

启用网页搜索可在 `mimi_secrets.h` 中设置 [Tavily API key](https://app.tavily.com/home)（优先，`MIMI_SECRET_TAVILY_KEY`），或 [Brave Search API key](https://brave.com/search/api/)（`MIMI_SECRET_SEARCH_KEY`）。

## 规则（自主控制循环）

规则引擎让 MimiClaw 能够**在没有 LLM 参与的情况下运行持续的传感器驱动控制循环** — 完美适用于"检测到运动时开灯"或"GPIO 4 为高电平时将 GPIO 5 置高"这类实时自动化场景。

规则在专用 FreeRTOS 任务中运行，周期性地评估条件。条件满足时直接执行动作（零 token 消耗）。规则支持：

- **触发器**：`gpio_read`（单引脚）、`gpio_read_all`（所有引脚）、`interval`（基于时间的无条件触发）
- **条件**：`==`、`!=`、`>`、`<`、`>=`、`<=`、`any_high`、`all_low`、`mod_eq`、`mod_ne`
- **动作**：`gpio_write`、`pwm_set`、`pwm_release`、`script_run`、`escalate`
- **否则动作**：条件变为 false 时执行（如运动停止时关灯）
- **冷却计时器**：防止规则抖动

规则持久化存储在 SPIFFS 的 `rules.json` 中，重启后不会丢失。LLM 可通过 `rule_*` 工具创建、列出、启用、禁用和删除规则。

**规则 JSON 示例**（用于 `rule_add`）：

```json
{
  "name": "motion_light",
  "interval_s": 5,
  "cooldown_s": 30,
  "trigger": {"type": "gpio_read", "pin": 4},
  "condition": {"op": "==", "value": 1},
  "actions": [{"type": "gpio_write", "pin": 5, "value": 1}],
  "else_actions": [{"type": "gpio_write", "pin": 5, "value": 0}]
}
```

## 定时任务（Cron）

MimiClaw 内置 cron 调度器，让 AI 可以自主安排任务。LLM 可以通过 `cron_add` 工具创建周期性任务（"每 N 秒"）或一次性任务（"在某个时间戳"）。任务触发时，消息会注入到 Agent 循环 — AI 自动醒来、处理任务并回复。

任务持久化存储在 SPIFFS（`cron.json`），重启后不会丢失。典型用途：每日总结、定时提醒、定期巡检。

## 心跳（Heartbeat）

心跳服务会定期读取 SPIFFS 上的 `HEARTBEAT.md`，检查是否有待办事项。如果发现未完成的条目（非空行、非标题、非已勾选的 `- [x]`），就会向 Agent 循环发送提示，让 AI 自主处理。

这让 MimiClaw 变成一个主动型助理 — 把任务写入 `HEARTBEAT.md`，机器人会在下一次心跳周期自动拾取执行（默认每 30 分钟）。

## RC 车 / 舵机校准

RC 车转向和油门通过 PWM 控制，从 `/spiffs/config/rc.json` 读取校准参数。所有字段均为可选，缺省值如下。

```json
{
  "steer_gpio": 4,
  "steer_center_us": 1500,
  "steer_min_us": 1000,
  "steer_max_us": 2000,
  "steer_reversed": false,
  "throttle_gpio": 5,
  "throttle_neutral_us": 1500,
  "throttle_forward_us": 2000,
  "throttle_reverse_us": 1000,
  "throttle_reversed": false
}
```

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `steer_gpio` | 4 | 转向舵机 GPIO 引脚 |
| `steer_center_us` | 1500 | 中立脉宽（µs） |
| `steer_min_us` | 1000 | 全左脉宽（µs） |
| `steer_max_us` | 2000 | 全右脉宽（µs） |
| `steer_reversed` | false | 翻转左右方向 |
| `throttle_gpio` | 5 | 油门 ESC GPIO 引脚 |
| `throttle_neutral_us` | 1500 | 停止/中立脉宽（µs） |
| `throttle_forward_us` | 2000 | 全速前进脉宽（µs） |
| `throttle_reverse_us` | 1000 | 全速倒退脉宽（µs） |
| `throttle_reversed` | false | 翻转前进/倒退方向 |

将此文件放在 `spiffs_data/config/rc.json` 可预烧录，或运行时通过 `write_file` 创建。

## 模块开关

部分模块可通过 `mimi_config.h` 中的宏在编译时启用/禁用：

| 宏 | 默认值 | 说明 |
|------|--------|------|
| `MIMI_GPIO_CONFIG_SECTION` | `1` | 启用 GPIO 工具（gpio_write / gpio_read / gpio_read_all） |
| `MIMI_PWM_CONFIG_SECTION` | `1` | 启用 PWM/RC 工具（pwm_set / pwm_release / rc_steer / rc_throttle） |
| `MIMI_SCRIPT_CONFIG_SECTION` | `1` | 启用脚本工具（script_create / script_run / script_list / script_remove） |
| `MIMI_RULE_CONFIG_SECTION` | `1` | 启用规则引擎（rule_add / rule_list / rule_remove / rule_enable / rule_disable） |
| `MIMI_TELEGRAM_CONFIG_SECTION` | `0` | 启用 Telegram 通道（设为 `1` 开启） |

修改后需重新编译：`idf.py fullclean && idf.py build`

## 其他功能

- **WebSocket 网关** — 端口 18789，局域网内用任意 WebSocket 客户端连接
- **OTA 更新** — WiFi 远程刷固件，无需 USB，CLI 和 AI 均可触发
- **双核** — 网络 I/O 和 AI 处理分别跑在不同 CPU 核心
- **HTTP 代理** — CONNECT 隧道，适配受限网络
- **多提供商** — 同时支持 Anthropic (Claude) 和 OpenAI (GPT)，运行时可切换
- **定时任务** — AI 可自主创建周期性和一次性任务，重启后持久保存
- **心跳服务** — 定期检查任务文件，驱动 AI 自主执行
- **规则引擎** — 无需 LLM 的传感器驱动自主控制循环，零 token 消耗
- **工具调用** — ReAct Agent 循环，两种提供商均支持工具调用

## 开发者

技术细节在 `docs/` 文件夹：

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — 系统设计、模块划分、任务布局、内存分配、协议、Flash 分区
- **[docs/TODO.md](docs/TODO.md)** — 功能差距和路线图
- **[docs/WIFI_ONBOARDING_AP.md](docs/WIFI_ONBOARDING_AP.md)** — 说明本地 `MimiClaw-XXXX` onboarding / 管理热点的使用方式
- **[docs/tool-setup/](docs/tool-setup/README.md)** — 外部服务集成配置指南（Tavily 等）

## 贡献

提交 Issue 或 Pull Request 前，请先阅读 **[CONTRIBUTING.md](CONTRIBUTING.md)**。

## 贡献者

感谢所有为 MimiClaw 做出贡献的开发者。

<a href="https://github.com/memovai/mimiclaw/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=memovai/mimiclaw" alt="MimiClaw contributors" />
</a>

## 许可证

MIT

## 致谢

灵感来自 [OpenClaw](https://github.com/openclaw/openclaw) 和 [Nanobot](https://github.com/HKUDS/nanobot)。MimiClaw 为嵌入式硬件重新实现了核心 AI Agent 架构 — 没有 Linux，没有服务器，只有一颗 $5 的芯片。

## Star History

<a href="https://www.star-history.com/?repos=memovai%2Fmimiclaw&type=date&legend=top-left">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/image?repos=memovai/mimiclaw&type=date&theme=dark&legend=top-left" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/image?repos=memovai/mimiclaw&type=date&legend=top-left" />
   <img alt="Star History Chart" src="https://api.star-history.com/image?repos=memovai/mimiclaw&type=date&legend=top-left" />
 </picture>
</a>
