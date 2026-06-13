# MimiClaw 项目结构与外部开源项目调研

## 1. 调研目标

本次调研目标是判断当前 GitHub 上哪些开源项目或官方仓库可以用于 MimiClaw 项目，具体包括三种使用方式：

1. 直接作为 ESP-IDF 组件或代码依赖集成。
2. 作为驱动、通信、OTA、配网、传感器融合等模块的实现参考。
3. 作为后续架构演进方向，例如 ROS 2/micro-ROS、嵌入式 AI Agent、机器人导航栈。

当前项目已经有较完整的 ESP32-S3 AI RC car 架构，因此本次调研重点不是替换现有系统，而是寻找能增强可靠性、可维护性、传感器能力和运维能力的项目。

## 2. 当前项目基本边界

### 2.1 硬件与构建边界

- 平台：ESP32-S3。
- 框架：ESP-IDF v5.5.x，`main/idf_component.yml` 要求 `>=5.5.0,<5.6.0`。
- 当前外部组件：`espressif/esp_websocket_client: ^1.4.0`。
- 构建入口：`main/CMakeLists.txt` 使用单一 `main` component 注册所有 C 源文件。
- 存储：NVS + SPIFFS。
- 分区：双 OTA app slot + 12 MB SPIFFS + coredump。

### 2.2 系统启动路径

`main/mimi.c` 中 `app_main()` 的核心初始化顺序：

1. NVS 初始化。
2. ESP event loop 初始化。
3. SPIFFS 挂载。
4. SPIFFS 离线日志初始化。
5. Message bus、memory、skill、session、WiFi、HTTP proxy 初始化。
6. Telegram、飞书、LLM proxy、工具注册、导航控制器、cron、heartbeat、rule engine、agent loop 初始化。
7. Serial CLI 启动。
8. WiFi 连接；失败则进入 WiFi onboarding。
9. WiFi 成功后启动 outbound、agent loop、IM 通道、cron、heartbeat、rule engine、WebSocket server、nav controller。

关键结论：当前项目是以 FreeRTOS 任务和 ESP-IDF 原生组件为核心组织方式，不适合直接引入大型框架替换主循环。

## 3. 当前模块结构分析

### 3.1 Agent 与 LLM 层

核心文件：

- `main/agent/agent_loop.c`
- `main/agent/context_builder.c`
- `main/llm/llm_proxy.c`
- `main/tools/tool_registry.c`

工作流：

1. 从 inbound queue 获取用户消息。
2. 设置当前消息来源，用于工具调用回传。
3. 构建 system prompt。
4. 加载 session history。
5. 调用 LLM。
6. 如果 LLM 返回 tool_use，则执行工具并把 tool_result 追加回消息。
7. 最多执行 `MIMI_AGENT_MAX_TOOL_ITER` 轮。
8. 保存用户消息与助手回复。
9. 推送 outbound queue。

实现特点：

- 大 buffer 使用 PSRAM 分配。
- 工具 schema 在启动时注册并缓存成 JSON。
- 工具执行接口统一为 `execute(input_json, output, output_size)`。
- 当前 Agent 适合继续扩展工具，但不适合引入依赖庞大的运行时。

增强机会：

- 增强 LLM 请求失败诊断。
- 增加工具调用审计日志。
- 增加 token/上下文大小统计。
- 借鉴嵌入式 LLM 示例中的 HTTPS、证书、内存优化模式，但不建议重写 Agent loop。

### 3.2 通信层

核心文件：

- `main/channels/telegram/telegram_bot.c`
- `main/channels/feishu/feishu_bot.c`
- `main/gateway/ws_server.c`
- `main/bus/message_bus.c`

结构：

- inbound queue：通道输入到 Agent。
- outbound queue：Agent 输出到通道分发。
- outbound dispatch 由 `mimi.c` 中任务统一处理。

增强机会：

- WebSocket 协议层可以参考 ESP-IDF 官方 HTTP server/WebSocket example。
- 通信错误可以进一步结构化，例如区分认证失败、网络失败、队列拥塞。
- 可增加本地 Web 管理 UI 的状态页和实时日志流。

### 3.3 WiFi、配网与 OTA

核心文件：

- `main/wifi/wifi_manager.c`
- `main/onboard/wifi_onboard.c`
- `main/ota/ota_manager.c`
- `main/tools/tool_ota.c`

现状：

- WiFi 凭据优先从 NVS 读取，其次使用 build-time secrets。
- WiFi 失败时进入 onboarding captive/admin portal。
- OTA 已使用 `esp_https_ota()`，并绑定 ESP x509 certificate bundle。

增强机会：

- OTA 当前是最小实现，可参考 ESP-IDF advanced HTTPS OTA example 增加：
  - 固件版本检查；
  - app rollback；
  - 签名校验；
  - 下载进度；
  - 分阶段日志。
- 配网可参考 ESP-IDF WiFi provisioning manager，评估是否保留当前 captive portal 或增加 BLE/SoftAP provisioning。

### 3.4 工具系统

核心文件：

- `main/tools/tool_registry.c`
- `main/tools/tool_*.c`

已有工具类型：

- Web search。
- 时间。
- SPIFFS 文件读写。
- cron。
- GPIO。
- PWM。
- script。
- rule。
- OTA。
- sensors。
- nav。

关键约束：

- `tool_nav_init()` 必须早于 `tool_sensors_init()`，因为 GPS 任务启动后可能立即调用 `nav_gps_filter_update()`。
- GPIO 访问受 `tools/gpio_policy.c` allowlist 约束。

增强机会：

- 工具 schema 可从静态字符串逐步转为更可维护的定义方式。
- 对高风险工具增加更明确的参数约束和状态前置检查。
- 对 sensors/nav 工具增加一次性诊断报告，方便 LLM 决策。

### 3.5 导航与传感器层

核心文件：

- `main/drivers/driver_ultrasonic.c`
- `main/drivers/driver_imu.c`
- `main/drivers/driver_gps.c`
- `main/nav/nav_situation.c`
- `main/nav/nav_gps_filter.c`
- `main/nav/nav_l1_reflex.c`
- `main/nav/nav_l2_fsm.c`
- `main/nav/nav_planner.c`
- `main/nav/nav_memory.c`
- `main/nav/nav_escalate.c`

现状：

- GPS 驱动内置 NMEA parser，处理 GPRMC/GPGGA。
- GPS 数据进入 `nav_gps_filter.c` 的 4 状态 Kalman filter。
- ENU 坐标以第一组有效 GPS fix 为原点。
- L1 反射层负责传感器情况更新。
- L2 FSM 负责巡航、避障、倒车、重规划、暂停、到达、故障等状态。
- 避障决策综合：左右超声波距离、目标航向收益、本地失败记忆惩罚。

增强机会：

- IMU 姿态融合可参考成熟 AHRS 库，例如 Madgwick/Mahony。
- GPS NMEA parser 可参考 TinyGPSPlus，但当前 C 实现已经够轻量，除非需要更多句型与字段。
- 若未来要与上位机/ROS 2 联动，可把当前 nav_situation 映射成 micro-ROS topic。
- 当前导航栈更像轻量嵌入式控制器，不建议直接引入完整无人车/ROS 导航算法到板端。

### 3.6 存储、日志与配置

核心文件：

- `main/log/spiffs_log.c`
- `main/memory/memory_store.c`
- `main/memory/session_mgr.c`
- `spiffs_data/config/*.json`
- `spiffs_data/memory/MEMORY.md`
- `spiffs_data/skills/*.md`

现状：

- SPIFFS 用于 AI 配置、技能、记忆、session、日志。
- NVS 用于 WiFi、API key、model、proxy 等运行时覆盖配置。
- 日志镜像到 SPIFFS，便于离线排错。

增强机会：

- 参考 Espressif log router / VFS 组件增强日志路由。
- 对日志读取工具增加分页、过滤、最近错误摘要。
- 对配置文件增加 schema 校验，减少现场配置错误。

## 4. 外部开源项目候选

### 4.1 Espressif ESP-IDF

仓库：`espressif/esp-idf`

用途：官方 ESP-IDF 框架与 examples。

与当前项目关系：

- 当前项目已经基于 ESP-IDF。
- 最值得参考的是 examples，而不是引入额外依赖。
- 重点参考方向：
  - OTA examples；
  - WiFi provisioning；
  - HTTP server / WebSocket；
  - SPIFFS / NVS；
  - system diagnostics。

适配建议：

- 短期：参考 advanced HTTPS OTA，增强当前 `ota_manager.c`。
- 中期：参考 HTTP server/WebSocket example，增强本地 admin portal 与 WebSocket 状态接口。
- 长期：把当前自定义 onboarding 与官方 provisioning manager 做一次对比评估。

结论：强推荐，作为官方参考来源。

### 4.2 Espressif ESP-IoT-Solution

仓库：`espressif/esp-iot-solution`

用途：Espressif 官方 IoT 组件集合，包含传感器、显示、输入、USB、BLE、驱动、FOC、OpenAI/MCP 等组件。

与当前项目关系：

- 当前项目有大量外设与 IoT 运维需求。
- 该仓库比通用 Arduino 库更贴近 ESP-IDF。

潜在可用点：

- I2C/SPI bus abstraction。
- servo / motor / BLDC / SimpleFOC 相关组件。
- USB、BLE OTA、TinyUF2、log router。
- OpenAI component、MCP C SDK、QuickJS-NG 等 AI/Agent 相关方向。
- 传感器驱动参考。

适配建议：

- 优先查看是否有组件能替换或增强当前手写外设驱动。
- 对电机控制组件应先做小样验证，不直接替换 `tool_pwm.c`。
- AI 相关组件可用于学习协议封装，但当前 `llm_proxy.c` 不宜贸然替换。

结论：强推荐，优先级最高的外部组件池。

### 4.3 micro-ROS ESP-IDF Component

仓库：`micro-ROS/micro_ros_espidf_component`

用途：在 ESP-IDF 上运行 micro-ROS，支持 ESP32、ESP32-S2、ESP32-S3、ESP32-C3、ESP32-C6，并覆盖 ESP-IDF v5.2 到 v5.5。

与当前项目关系：

- 当前项目是独立嵌入式机器人控制器。
- 如果未来需要与 ROS 2 上位机、仿真、SLAM、可视化工具集成，micro-ROS 是较合适的桥接方式。

适配建议：

- 不建议短期直接引入主线固件。
- 建议新建实验分支或示例 component：
  - 发布 `nav_situation` 为 topic；
  - 发布传感器数据；
  - 订阅目标点或控制命令；
  - 保持现有 L2 FSM 为板端安全控制层。

风险：

- README 明确提示不适合生产使用。
- 需要 micro-ROS Agent。
- 会增加构建复杂度、内存占用和运行时依赖。

结论：中长期推荐，适合 ROS 2 联动，不适合立即深度集成。

### 4.4 TinyGPSPlus

仓库：`mikalhart/TinyGPSPlus`

用途：Arduino 生态常用 NMEA parser，能解析位置、时间、海拔、速度、航向和自定义字段。

与当前项目关系：

- 当前项目已有轻量 C NMEA parser。
- 如果后续需要更多 NMEA 句型、字段或更成熟的解析行为，可以参考 TinyGPSPlus 的 API 和测试思路。

适配建议：

- 不建议直接引入到当前 C 工程，除非接受 C++ component 或包装层。
- 可借鉴其字段抽象和自定义 NMEA field 机制。
- 当前项目更适合继续维护轻量 C parser，并为 GPRMC/GPGGA 增加测试样例。

结论：参考设计，不建议直接集成。

### 4.5 jrowberg/i2cdevlib

仓库：`jrowberg/i2cdevlib`

用途：多平台 I2C 设备驱动集合，包含 MPU6050 等大量 IMU/传感器支持。

与当前项目关系：

- 当前使用 MPU6050。
- 该仓库有成熟 MPU6050 相关代码与示例。

适配建议：

- 用于对照当前 `driver_imu.c` 的寄存器配置、量程、校准和 DMP 使用方式。
- 如果要引入 DMP 或更完整的 MPU6050 功能，可先在隔离实验中验证。
- 不建议直接拷贝大量 Arduino/C++ 代码进主线。

结论：适合参考 IMU 驱动细节，不建议直接集成。

### 4.6 Adafruit AHRS

仓库：`adafruit/Adafruit_AHRS`

用途：提供 Mahony、Madgwick、NXP/Kalman 等 AHRS 姿态融合算法。

与当前项目关系：

- 当前导航依赖 yaw，且项目文档提到磁力计提供绝对 yaw。
- AHRS 算法可提升 IMU 姿态估计稳定性。

适配建议：

- 优先评估 Mahony/Madgwick 的核心算法是否能以小型 C/C++ 模块方式移植。
- 不建议引入完整 Arduino library；建议移植算法核心或自行实现同等滤波器。
- 增加磁力计校准流程与日志，否则 AHRS 效果会受环境干扰。

结论：强参考，适合增强姿态融合。

### 4.7 Bolder Flight invensense-imu

仓库：`bolderflight/invensense-imu`

用途：MPU-6500、MPU-9250、MPU-9255 IMU C++ 驱动，支持 I2C/SPI、量程、DLPF、数据就绪中断、磁力计等。

与当前项目关系：

- 如果未来从 MPU6050 升级到 MPU9250/9255 或带磁力计的 InvenSense IMU，该项目有参考价值。

适配建议：

- 作为新 IMU 选型和驱动实现参考。
- ESP-IDF 下需要验证 CMake、bus 初始化和 C++ ABI。
- 当前硬件若仍是 MPU6050，则优先级低于 i2cdevlib 和 AHRS。

结论：中等推荐，主要用于未来 IMU 升级。

### 4.8 nopnop2002 ESP-IDF 外设示例系列

代表仓库：`nopnop2002/esp-idf-ssd1306`

用途：ESP-IDF 外设驱动和示例项目，尤其擅长用 menuconfig 暴露硬件配置。

与当前项目关系：

- 当前项目大量配置在 `spiffs_data/config/*.json` 和编译期默认值中。
- 该类项目的结构适合参考外设驱动组织方式，而不是直接集成 SSD1306。

适配建议：

- 借鉴其 Kconfig/menuconfig 组织方式，特别是硬件引脚、总线、设备型号配置。
- 对 MimiClaw 来说，运行时 JSON 配置仍更适合现场改配置；Kconfig 更适合编译期硬件变体。

结论：参考工程组织方式。

### 4.9 arduino-audio-tools

仓库：`pschatzmann/arduino-audio-tools`

用途：ESP32/Arduino/IDF 音频流、I2S、DSP、codec、播放和录音工具库。

与当前项目关系：

- 当前项目不是语音机器人，但未来如果增加语音输入/播报/音频提示，该项目有价值。

适配建议：

- 当前阶段不建议引入。
- 若未来做语音 Agent，可先作为语音 I/O 原型库评估。

结论：低优先级，未来语音功能候选。

## 5. 项目适配总体判断

### 5.1 最值得优先使用的来源

1. `espressif/esp-idf` examples：官方基线，适合增强 OTA、WiFi、HTTP/WebSocket、NVS/SPIFFS。
2. `espressif/esp-iot-solution`：ESP-IDF 组件池，适合外设、IoT 运维、AI/Agent 组件参考。
3. `Adafruit_AHRS`：适合姿态融合算法增强。
4. `micro_ros_espidf_component`：适合未来 ROS 2 联动。

### 5.2 不建议直接采用的模式

- 大量 Arduino-only RC car 项目：通常结构简单、缺少 FreeRTOS/ESP-IDF 任务模型，迁移成本高。
- 直接替换当前导航 FSM：当前项目已有针对 RC car 传感器和 LLM escalations 的状态机，不应轻易替换。
- 在主固件中直接引入 micro-ROS：应先实验验证内存、任务调度和网络影响。
- 为了“复用”而引入大型 C++ 库：当前主工程是 C/ESP-IDF 风格，应控制 ABI 和依赖复杂度。

## 6. 当前项目最有价值的增强方向

1. OTA 安全与可靠性增强。
2. IMU/AHRS 姿态融合增强。
3. WiFi provisioning 与本地 admin portal 改进。
4. WebSocket 状态监控和诊断接口增强。
5. GPS parser 与 GPS filter 测试样例补强。
6. micro-ROS 实验桥接。
7. 工具调用审计与高风险工具保护。

## 7. 资料来源

- https://github.com/espressif/esp-idf
- https://github.com/espressif/esp-iot-solution
- https://github.com/micro-ROS/micro_ros_espidf_component
- https://github.com/mikalhart/TinyGPSPlus
- https://github.com/jrowberg/i2cdevlib
- https://github.com/adafruit/Adafruit_AHRS
- https://github.com/bolderflight/invensense-imu
- https://github.com/nopnop2002/esp-idf-ssd1306
- https://github.com/pschatzmann/arduino-audio-tools
