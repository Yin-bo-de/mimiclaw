# CLI 命令参考手册

Mimi 串口 REPL 提供了对设备配置、传感器调试、日志管理等功能的完整控制接口。

## 快速上手

### 连接方式

使用 **COM (UART) 端口** 连接串口终端，波特率 115200：

```
screen /dev/cu.usbserial-* 115200
```

> **注意**：USB (JTAG) 端口（`/dev/cu.usbmodem*`）仅用于 `idf.py flash` 和调试，**不支持 REPL 交互输入**。
> REPL 运行在 UART 端口，因为 `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`。

### 提示符与参数规则

- 提示符：`mimi> `
- 参数解析：基于 argtable3，支持短选项（`-s`）和长选项（`--speed`）
- 必填参数用 `<...>` 表示，可选参数用 `[...]` 表示
- 字符串参数含空格时需要用引号包裹
- 内置 `help` 命令可列出所有命令及用法

### 配置优先级

配置项有三级来源，优先级从高到低：

1. **NVS 运行时覆盖**（通过 CLI 命令设置，重启后仍有效）
2. **编译时默认值**（`mimi_config.h` / `mimi_secrets.h`）
3. **SPIFFS JSON 配置**（`rc.json`、`sensors.json` 等）

使用 `config_show` 可查看每项配置的当前值及来源（`[NVS]` 或 `[build]`）。

---

## 命令分类速查

| 分类 | 命令 |
|------|------|
| 系统管理 | `restart`, `heap_info`, `config_show`, `config_reset` |
| WiFi | `set_wifi`, `wifi_status`, `wifi_scan` |
| 即时通讯 | `set_tg_token`, `set_feishu_creds`, `feishu_send` |
| LLM 配置 | `set_api_key`, `set_model`, `set_model_provider`, `set_api_url`, `set_api_host` |
| 代理 | `set_proxy`, `clear_proxy` |
| 搜索 API Key | `set_search_key`, `set_tavily_key`, `set_bing_key` |
| 记忆与会话 | `memory_read`, `memory_write`, `session_list`, `session_clear` |
| 技能 | `skill_list`, `skill_show`, `skill_search` |
| 心跳与定时 | `heartbeat_trigger`, `cron_start` |
| 导航测试 | `l1_test` |
| 传感器测试 | `ultrasonic_test`, `imu_test`, `imu_calibrate`, `gps_test`, `gps_nmea`, `gps_filt_stats`, `gps_filt_reset`, `mag_status`, `mag_cal`, `mag_decl`, `mag_heading_offset` |
| 工具与网络 | `tool_exec`, `web_search`, `ota_update` |
| 日志 | `log_list`, `log_read`, `log_delete`, `log_clear`, `log_status` |

---

## 命令详解

### 系统管理

#### `restart`

重启设备。

```
mimi> restart
```

#### `heap_info`

显示堆内存使用情况（内部 RAM + PSRAM）。

```
mimi> heap_info
```

输出示例：
```
Internal free: 123456 bytes
PSRAM free:    2345678 bytes
Total free:    2469134 bytes
```

#### `config_show`

显示当前所有配置项的值及来源。敏感字段（密码、API Key 等）仅显示前 4 位 + `****`。

```
mimi> config_show
```

显示内容包括：WiFi SSID/密码、Telegram Token、API Key、Model、Provider、API URL/Host、代理配置、搜索 API Key 等。

#### `config_reset`

清除所有 NVS 运行时覆盖，恢复为编译时默认值。**重启后生效**。

```
mimi> config_reset
```

> **注意**：此操作不可逆，会清除 WiFi、API Key、代理等所有 CLI 设置的值。

---

### WiFi

#### `set_wifi`

设置 WiFi SSID 和密码，保存到 NVS。**重启后生效**。

```
mimi> set_wifi <ssid> <password>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<ssid>` | 是 | WiFi 名称 |
| `<password>` | 是 | WiFi 密码 |

示例：
```
mimi> set_wifi MyHomeWiFi MyPassword123
```

#### `wifi_status`

查看当前 WiFi 连接状态和 IP 地址。

```
mimi> wifi_status
```

#### `wifi_scan`

扫描附近 WiFi 热点并列出。

```
mimi> wifi_scan
```

---

### 即时通讯

#### `set_tg_token`

设置 Telegram Bot Token，保存到 NVS。

```
mimi> set_tg_token <token>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<token>` | 是 | Telegram Bot Token |

> **注意**：仅当编译时启用 `MIMI_TELEGRAM_CONFIG_SECTION` 宏时此命令才可用。

示例：
```
mimi> set_tg_token 123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11
```

#### `set_feishu_creds`

设置飞书应用凭据（App ID + App Secret），保存到 NVS。

```
mimi> set_feishu_creds <app_id> <app_secret>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<app_id>` | 是 | 飞书应用 App ID |
| `<app_secret>` | 是 | 飞书应用 App Secret |

示例：
```
mimi> set_feishu_creds cli_a1b2c3d4 MySecretKey567
```

#### `feishu_send`

发送飞书文本消息（用于测试通道连通性）。

```
mimi> feishu_send <receive_id> <text>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<receive_id>` | 是 | 接收者的 open_id 或 chat_id |
| `<text>` | 是 | 消息文本（含空格需加引号） |

示例：
```
mimi> feishu_send ou_xxxxx "Hello from Mimi!"
```

---

### LLM 配置

#### `set_api_key`

设置 LLM API Key，保存到 NVS。

```
mimi> set_api_key <key>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<key>` | 是 | Anthropic / OpenAI API Key |

示例：
```
mimi> set_api_key sk-ant-api03-xxxxxxxxx
```

#### `set_model`

设置 LLM 模型名称。

```
mimi> set_model <model>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<model>` | 是 | 模型标识符 |

示例：
```
mimi> set_model claude-3-sonnet-20240229
mimi> set_model gpt-4o
```

#### `set_model_provider`

设置 LLM 提供商（决定 API 调用格式）。

```
mimi> set_model_provider <provider>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<provider>` | 是 | `anthropic` 或 `openai` |

示例：
```
mimi> set_model_provider anthropic
mimi> set_model_provider openai
```

#### `set_api_url`

设置自定义 LLM API URL（用于 OpenAI 兼容的第三方服务）。

```
mimi> set_api_url <url>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<url>` | 是 | 完整 API URL |

示例：
```
mimi> set_api_url https://ark.cn-beijing.volces.com/api/v3/chat/completions
```

#### `set_api_host`

设置自定义 LLM API Host（用于 HTTP 代理 CONNECT 隧道）。

```
mimi> set_api_host <host>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<host>` | 是 | API 主机名（不含协议和路径） |

示例：
```
mimi> set_api_host ark.cn-beijing.volces.com
```

---

### 代理

#### `set_proxy`

设置 HTTP/SOCKS5 代理。**重启后生效**。

```
mimi> set_proxy <host> <port> [<type>]
```

| 参数 | 必填 | 默认值 | 说明 |
|------|------|--------|------|
| `<host>` | 是 | - | 代理服务器 IP 或域名 |
| `<port>` | 是 | - | 代理端口 |
| `<type>` | 否 | `http` | 代理类型：`http` 或 `socks5` |

示例：
```
mimi> set_proxy 192.168.1.83 7897
mimi> set_proxy 192.168.1.83 1080 socks5
```

#### `clear_proxy`

移除代理配置。**重启后生效**。

```
mimi> clear_proxy
```

---

### 搜索 API Key

#### `set_search_key`

设置 Brave Search API Key，供 `web_search` 工具使用。

```
mimi> set_search_key <key>
```

#### `set_tavily_key`

设置 Tavily Search API Key，供 `web_search` 工具使用。

```
mimi> set_tavily_key <key>
```

#### `set_bing_key`

设置 Bing Search API Key，供 `web_search` 工具使用。

```
mimi> set_bing_key <key>
```

> 三个搜索 Key 设置一个即可，`web_search` 工具会按优先级依次尝试。

---

### 记忆与会话

#### `memory_read`

读取 `/spiffs/memory/MEMORY.md` 的内容。

```
mimi> memory_read
```

#### `memory_write`

写入内容到 `/spiffs/memory/MEMORY.md`（覆盖写入）。

```
mimi> memory_write <content>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<content>` | 是 | 要写入的文本内容 |

示例：
```
mimi> memory_write "User prefers concise responses"
```

#### `session_list`

列出 `/spiffs/sessions/` 下所有会话历史文件。

```
mimi> session_list
```

#### `session_clear`

清除指定会话的历史记录。

```
mimi> session_clear <chat_id>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<chat_id>` | 是 | 要清除的会话 ID（如 `tg_123456`） |

示例：
```
mimi> session_clear tg_123456789
```

---

### 技能

#### `skill_list`

列出 `/spiffs/skills/` 下所有已安装的技能文件及摘要。

```
mimi> skill_list
```

#### `skill_show`

打印指定技能文件的完整内容。

```
mimi> skill_show <name>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<name>` | 是 | 技能名称（可带或不带 `.md` 后缀） |

示例：
```
mimi> skill_show weather
mimi> skill_show weather.md
```

#### `skill_search`

在技能文件（文件名 + 内容）中搜索关键词。

```
mimi> skill_search <keyword>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<keyword>` | 是 | 搜索关键词（不区分大小写） |

示例：
```
mimi> skill_search translate
```

---

### 心跳与定时

#### `heartbeat_trigger`

手动触发一次心跳检查（读取 `HEARTBEAT.md`，如有待办任务则推送给 Agent）。

```
mimi> heartbeat_trigger
```

#### `cron_start`

立即启动 cron 定时调度器。

```
mimi> cron_start
```

---

### 导航测试

#### `l1_test`

L1 反射层测试：驱动车辆前进，遇障碍物自动停止。移除障碍物后自动恢复行驶。

```
mimi> l1_test [-s <pct>] [-x]
```

| 参数 | 必填 | 默认值 | 说明 |
|------|------|--------|------|
| `-s` / `--speed` | 否 | 20 | 前进速度百分比（1-100） |
| `-x` / `--stop` | 否 | - | 停止当前测试行驶 |

行为说明：
- L1 反射层会在任意传感器 < 20cm 时强制停车
- 障碍物移除后自动恢复
- 已在运行时再次启动会报错，需先 `l1_test -x` 停止

示例：
```
mimi> l1_test
mimi> l1_test -s 35
mimi> l1_test -x
```

---

### 传感器测试

#### `ultrasonic_test`

测试 HC-SR04 超声波传感器（左/前/右三路）。

```
mimi> ultrasonic_test [-c <n>] [-d <ms>]
```

| 参数 | 必填 | 默认值 | 说明 |
|------|------|--------|------|
| `-c` / `--count` | 否 | 10 | 采样次数 |
| `-d` / `--delay` | 否 | 1000 | 采样间隔（毫秒） |

输出包含：距离(cm)、数据有效性(valid)、数据年龄(ms)。

示例：
```
mimi> ultrasonic_test
mimi> ultrasonic_test -c 20 -d 500
```

#### `imu_test`

测试 MPU6050 IMU 传感器（Roll/Pitch/Yaw + 加速度 + 陀螺仪）。

```
mimi> imu_test [-c <n>] [-d <ms>]
```

| 参数 | 必填 | 默认值 | 说明 |
|------|------|--------|------|
| `-c` / `--count` | 否 | 10 | 采样次数 |
| `-d` / `--delay` | 否 | 1000 | 采样间隔（毫秒） |

输出包含：Roll/Pitch/Yaw(°)、加速度(g)、角速度(°/s)、数据有效性、数据年龄(ms)。

示例：
```
mimi> imu_test -c 5 -d 2000
```

#### `imu_calibrate`

重新校准 IMU 陀螺仪零偏。校准时**必须保持板子静止 5 秒**。

```
mimi> imu_calibrate
```

校准结果保存到 `sensors.json`，重启后生效。

#### `gps_test`

测试 NEO-6M GPS 传感器。

```
mimi> gps_test [-c <n>] [-d <ms>] [-v]
```

| 参数 | 必填 | 默认值 | 说明 |
|------|------|--------|------|
| `-c` / `--count` | 否 | 10 | 采样次数 |
| `-d` / `--delay` | 否 | 1000 | 采样间隔（毫秒） |
| `-v` / `--verbose` | 否 | 关闭 | 同时输出原始 NMEA 语句 |

输出包含：经纬度、海拔(m)、速度(m/s)、航向(°)、卫星数、定位有效性、数据年龄(ms)。

示例：
```
mimi> gps_test -c 5 -v
```

#### `gps_nmea`

输出原始 NMEA 语句（用于 GPS 模块调试）。

```
mimi> gps_nmea [-d <ms>]
```

| 参数 | 必填 | 默认值 | 说明 |
|------|------|--------|------|
| `-d` / `--duration` | 否 | 10000 | 输出持续时间（毫秒） |

示例：
```
mimi> gps_nmea -d 5000
```

#### `gps_filt_stats`

显示 GPS 卡尔曼滤波器统计信息。

```
mimi> gps_filt_stats
```

输出包含：初始化状态、ENU 原点坐标、总观测数、接受/弱化/拒绝数量、纯预测连续帧数、静止检测。

#### `gps_filt_reset`

重置 GPS 卡尔曼滤波器状态和 ENU 坐标原点。

```
mimi> gps_filt_reset
```

> 重置后，下次有效 GPS 定位将重新建立 ENU 原点。

#### `mag_status`

显示 HMC5883L 磁力计状态和当前航向。

```
mimi> mag_status
```

输出包含：在线状态、原始三轴数据(mG)、航向(°)、磁偏角、航向偏移、硬铁偏移。

#### `mag_cal`

校准磁力计硬铁偏移。校准时需要**将车辆水平放置，缓慢旋转 360°**（约 15 秒）。

```
mimi> mag_cal
```

校准结果保存到 `sensors.json`。

#### `mag_decl`

设置地磁偏角。**重启后生效**。

```
mimi> mag_decl <deg>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<deg>` | 是 | 磁偏角（度），西偏为负 |

示例：
```
mimi> mag_decl -7.0
```

#### `mag_heading_offset`

设置磁力计航向偏移（用于修正安装角度偏差）。**重启后生效**。

```
mimi> mag_heading_offset <deg>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<deg>` | 是 | 航向偏移（度），正值叠加到传感器航向 |

示例：
```
mimi> mag_heading_offset 15.0
```

---

### 工具与网络

#### `tool_exec`

直接执行已注册的工具（跳过 Agent，手动调用）。

```
mimi> tool_exec <name> [json]
```

| 参数 | 必填 | 默认值 | 说明 |
|------|------|--------|------|
| `<name>` | 是 | - | 工具名称（需与注册名完全一致） |
| `[json]` | 否 | `{}` | 工具输入参数（JSON 格式） |

示例：
```
mimi> tool_exec pwm_drive
mimi> tool_exec pwm_drive '{"throttle":30,"steer":0}'
mimi> tool_exec get_location
```

#### `web_search`

直接调用网页搜索工具（超时 45 秒）。

```
mimi> web_search <query>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<query>` | 是 | 搜索关键词 |

> 需要先通过 `set_search_key` / `set_tavily_key` / `set_bing_key` 之一配置搜索 API Key。

示例：
```
mimi> web_search "latest esp-idf release"
```

#### `ota_update`

通过 HTTPS URL 进行 OTA 固件升级。成功后设备自动重启，超时 130 秒。

```
mimi> ota_update <url>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<url>` | 是 | 固件 `.bin` 文件的 HTTPS URL |

示例：
```
mimi> ota_update https://example.com/firmware/mimiclaw-v2.bin
```

> **警告**：OTA 失败不会影响当前固件（A/B 分区机制），但过程中切勿断电。

---

### 日志

#### `log_list`

列出 `/spiffs/logs/` 下所有日志文件及大小。

```
mimi> log_list
```

#### `log_read`

打印指定日志文件内容到串口。

```
mimi> log_read <file>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<file>` | 是 | 日志文件名（如 `run_0001.log`） |

示例：
```
mimi> log_read run_0001.log
```

#### `log_delete`

删除指定日志文件。

```
mimi> log_delete <file>
```

| 参数 | 必填 | 说明 |
|------|------|------|
| `<file>` | 是 | 要删除的日志文件名 |

示例：
```
mimi> log_delete run_0001.log
```

#### `log_clear`

删除**所有**日志文件，释放 SPIFFS 空间。

```
mimi> log_clear
```

> **注意**：此操作不可逆。

#### `log_status`

显示 SPIFFS 使用情况和当前日志文件信息。

```
mimi> log_status
```

---

## 常用工作流

### 1. 开机首次配置

```
mimi> set_wifi MySSID MyPassword
mimi> set_api_key sk-ant-api03-xxxxxxxxx
mimi> set_model claude-3-sonnet-20240229
mimi> set_proxy 192.168.1.83 7897
mimi> config_show
mimi> restart
```

### 2. 传感器联调

```
mimi> ultrasonic_test -c 5          # 检查超声波
mimi> imu_test -c 5                 # 检查 IMU
mimi> imu_calibrate                 # 校准陀螺仪（保持静止 5s）
mimi> mag_status                    # 检查磁力计
mimi> mag_cal                       # 校准磁力计（旋转 360°）
mimi> gps_test -c 3 -v             # 检查 GPS（-v 输出原始 NMEA）
mimi> l1_test -s 20                 # 低速前进测试避障
mimi> l1_test -x                    # 停止测试
```

### 3. OTA 固件升级

```
mimi> ota_update https://example.com/firmware/mimiclaw-v2.bin
```

成功后设备自动重启。如需回滚，可通过 `idf.py flash` 刷回旧固件。

### 4. 日志排查

```
mimi> log_list                      # 查看有哪些日志
mimi> log_read run_0001.log         # 读取最新日志
mimi> log_status                    # 检查 SPIFFS 剩余空间
mimi> log_delete run_0001.log       # 删除不再需要的日志
mimi> log_clear                     # 清空所有日志（谨慎）
```

### 5. 搜索功能配置

```
mimi> set_search_key BSAXXXXXX      # Brave Search（推荐）
mimi> set_tavily_key tvly-xxxxx     # Tavily（备选）
mimi> set_bing_key xxxxxxxx         # Bing（备选）
mimi> web_search "test query"       # 验证搜索功能
```

---

## 附录

### A. NVS 存储命名空间

| 命名空间 | 存储内容 |
|----------|----------|
| `wifi` | SSID、密码 |
| `tg` | Telegram Bot Token |
| `feishu` | App ID、App Secret |
| `llm` | API Key、Model、Provider、API URL、API Host |
| `proxy` | 代理 Host、Port、Type |
| `search` | Brave Key、Tavily Key、Bing Key |

### B. 需重启生效的命令

以下命令修改 NVS 后需要 `restart` 才能生效：

- `set_wifi`
- `set_proxy` / `clear_proxy`
- `mag_decl`
- `mag_heading_offset`

以下命令修改 SPIFFS JSON 后重启生效：

- `imu_calibrate`（修改 `sensors.json`）

### C. 条件编译命令

| 命令 | 编译宏 | 说明 |
|------|--------|------|
| `set_tg_token` | `MIMI_TELEGRAM_CONFIG_SECTION` | 未启用时命令不存在 |

### D. 日志轮转规则

- 日志按启动次数编号：`run_0001.log`、`run_0002.log`...
- 单文件最大 512 KB
- 存储路径：`/spiffs/logs/`

### E. 危险操作提醒

| 命令 | 风险 |
|------|------|
| `config_reset` | 清除所有 NVS 配置，不可逆 |
| `log_clear` | 删除所有日志文件，不可逆 |
| `ota_update` | 固件升级，过程中不可断电 |
| `restart` | 重启设备，所有未保存状态丢失 |
