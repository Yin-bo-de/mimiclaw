# MimiClaw 开源项目适配规划

## 1. 规划原则

### 1.1 根本边界条件

MimiClaw 不是普通 Arduino 小车示例，而是运行在 ESP32-S3 上的 ESP-IDF AI Agent + RC car 控制系统。当前系统已经具备：

- FreeRTOS 多任务结构；
- 双队列消息总线；
- LLM ReAct tool calling；
- Telegram、飞书、WebSocket 通道；
- GPS、IMU、超声波传感器；
- L1/L2 导航栈；
- SPIFFS/NVS 持久化；
- OTA、日志、cron、rule engine。

因此外部开源项目的适配原则是：

1. 优先增强现有模块，而不是推倒重写。
2. 优先选择 ESP-IDF 原生项目，其次才考虑 Arduino/C++ 项目。
3. 对导航、安全、OTA、工具执行等高风险模块，必须先做小范围实验和测试。
4. 保持当前任务时序与初始化顺序稳定，尤其是导航和传感器任务。
5. 所有新增依赖必须评估许可证、内存占用、构建复杂度和维护状态。

## 2. 推荐项目分级

## 2.1 第一优先级：立即值得参考或局部集成

### A. Espressif ESP-IDF examples

仓库：`https://github.com/espressif/esp-idf`

推荐用途：官方基线参考。

适配模块：

- `main/ota/ota_manager.c`
- `main/onboard/wifi_onboard.c`
- `main/wifi/wifi_manager.c`
- `main/gateway/ws_server.c`
- `main/log/spiffs_log.c`

建议动作：

1. 对照 advanced HTTPS OTA example，增强 OTA：
   - 固件版本检查；
   - rollback 支持；
   - OTA 状态记录；
   - 下载进度日志；
   - 错误原因细分。
2. 对照 WiFi provisioning manager，评估当前 captive/admin portal 是否需要补 BLE/SoftAP provisioning。
3. 对照 HTTP server/WebSocket example，增强本地 admin portal 和 WebSocket 状态接口。
4. 对照 SPIFFS/NVS examples，为配置文件增加更明确的错误恢复逻辑。

预期收益：

- 提升 OTA 安全性和可恢复性。
- 降低现场配网失败率。
- 增强诊断能力。

风险：

- OTA rollback 会影响分区状态和启动路径，必须实机测试。
- provisioning manager 可能与当前自定义 onboarding 有重叠，需要避免两套配网状态机冲突。

建议结论：立即参考，按模块逐步增强。

### B. Espressif ESP-IoT-Solution

仓库：`https://github.com/espressif/esp-iot-solution`

推荐用途：ESP-IDF 组件池和工程组织参考。

适配模块：

- `main/drivers/*`
- `main/tools/tool_pwm.c`
- `main/log/spiffs_log.c`
- `main/llm/llm_proxy.c`
- `main/tools/tool_registry.c`

建议动作：

1. 检查是否有可复用的 I2C/SPI bus abstraction，评估替代当前散落的 bus 初始化逻辑。
2. 查看 servo/motor/SimpleFOC 相关组件，评估是否能改善 RC PWM 控制精度。
3. 查看 OpenAI component、MCP C SDK、QuickJS-NG，仅作为 LLM/Agent 能力参考。
4. 查看 log router、USB、BLE OTA、TinyUF2 等组件是否能增强运维体验。

预期收益：

- 更接近 ESP-IDF 官方生态。
- 减少自研外设驱动和工具链代码量。
- 为后续硬件扩展提供组件来源。

风险：

- 组件较多，不能盲目引入。
- motor/FOC 类组件可能不适配当前 RC servo + ESC 模型。
- AI 组件可能与现有 `llm_proxy.c` 设计冲突。

建议结论：高优先级调研，按单组件实验，不整仓引入。

### C. Adafruit AHRS

仓库：`https://github.com/adafruit/Adafruit_AHRS`

推荐用途：姿态融合算法参考。

适配模块：

- `main/drivers/driver_imu.c`
- `main/nav/nav_situation.c`
- `main/nav/nav_l2_fsm.c`

建议动作：

1. 先只评估 Mahony 和 Madgwick 两种滤波算法。
2. 在当前 IMU 读取任务中增加可切换的姿态融合路径。
3. 使用日志对比当前 yaw 与 AHRS yaw：
   - 静止漂移；
   - 低速转向响应；
   - 磁干扰环境稳定性。
4. 若效果明显，再把 AHRS 融合结果接入 `nav_situation`。

预期收益：

- 改善 yaw 稳定性。
- 降低导航 PID 因航向噪声产生的抖动。
- 为后续更复杂导航打基础。

风险：

- Arduino library 不能直接无脑引入。
- 磁力计校准不足会导致绝对 yaw 错误。
- 计算频率和任务优先级需要验证。

建议结论：强推荐作为算法参考，优先移植核心算法而不是整库集成。

## 2.2 第二优先级：适合专项增强或未来扩展

### D. micro-ROS ESP-IDF Component

仓库：`https://github.com/micro-ROS/micro_ros_espidf_component`

推荐用途：ROS 2 联动实验。

适配模块：

- `main/nav/nav_situation.c`
- `main/nav/nav_controller.c`
- `main/drivers/*`
- 可新增 `main/ros_bridge/*`

建议动作：

1. 不进入主线固件，先创建实验分支。
2. 最小实验：发布 `nav_situation` 快照 topic。
3. 第二阶段：发布 ultrasonic、GPS、IMU topic。
4. 第三阶段：订阅目标点或速度控制 topic。
5. 保持当前 L2 FSM 为安全控制边界，不让 ROS 直接绕过本地避障。

预期收益：

- 可接入 ROS 2 工具链、可视化、仿真和上位机规划。
- 为更高级路径规划留接口。

风险：

- micro-ROS Agent 依赖上位机。
- README 标注不适合生产使用。
- 增加内存、网络、构建复杂度。

建议结论：未来实验方向，不建议短期并入主线。

### E. TinyGPSPlus

仓库：`https://github.com/mikalhart/TinyGPSPlus`

推荐用途：NMEA parser 设计参考。

适配模块：

- `main/drivers/driver_gps.c`
- `main/nav/nav_gps_filter.c`

建议动作：

1. 对比当前 parser 支持的 GPRMC/GPGGA 字段。
2. 借鉴 TinyGPSPlus 的自定义字段机制。
3. 给当前 GPS parser 增加样例驱动测试数据。
4. 若未来需要更多 NMEA 句型，再评估 C++ wrapper。

预期收益：

- 提高 GPS parser 鲁棒性。
- 减少 NMEA 边界格式导致的导航异常。

风险：

- Arduino/C++ 风格与当前 C 工程不一致。
- 直接引入可能增加构建复杂度。

建议结论：参考，不建议直接集成。

### F. jrowberg/i2cdevlib

仓库：`https://github.com/jrowberg/i2cdevlib`

推荐用途：MPU6050 驱动和 DMP 参考。

适配模块：

- `main/drivers/driver_imu.c`

建议动作：

1. 对照 MPU6050 初始化寄存器配置。
2. 对照量程、采样率、低通滤波、校准流程。
3. 如果要使用 DMP，先做独立实验，确认内存和稳定性。

预期收益：

- 完善 IMU 初始化和校准。
- 为姿态融合提供更干净的原始数据。

风险：

- 项目为大型 monorepo，直接引入不划算。
- 示例多为 Arduino/C++。

建议结论：参考驱动细节，不直接引入。

### G. Bolder Flight invensense-imu

仓库：`https://github.com/bolderflight/invensense-imu`

推荐用途：未来 MPU9250/9255/6500 升级参考。

适配模块：

- `main/drivers/driver_imu.c`
- `main/nav/nav_situation.c`

建议动作：

1. 如果硬件升级到带磁力计 InvenSense IMU，评估该库。
2. 在独立 component 中验证 CMake + ESP-IDF C++ 编译。
3. 对比数据坐标系与当前 ENU/yaw 使用方式。

预期收益：

- 更完整的 IMU + magnetometer 驱动能力。

风险：

- README 测试平台偏 Teensy/Arduino，需要 ESP32-S3 验证。
- 坐标系可能与当前导航定义不同。

建议结论：硬件升级时再评估。

## 2.3 第三优先级：低优先或仅供参考

### H. nopnop2002 ESP-IDF 示例系列

代表仓库：`https://github.com/nopnop2002/esp-idf-ssd1306`

推荐用途：ESP-IDF 外设工程组织参考。

建议动作：

- 学习其 Kconfig/menuconfig、外设 demo、硬件差异文档组织方式。
- 如果 MimiClaw 后续增加 OLED/e-paper 小屏，可参考其驱动组织风格。

建议结论：参考工程风格，不作为当前核心依赖。

### I. arduino-audio-tools

仓库：`https://github.com/pschatzmann/arduino-audio-tools`

推荐用途：未来语音 I/O。

建议动作：

- 当前不引入。
- 未来如果增加语音播报、I2S 麦克风、离线唤醒词、声音提示，再作为候选库评估。

建议结论：未来语音功能候选，当前低优先级。

## 3. 推荐实施路线图

### 阶段 1：低风险增强，优先改善可靠性

目标：增强现有系统，不改变核心架构。

任务：

1. OTA 增强设计
   - 对照 ESP-IDF advanced HTTPS OTA。
   - 增加版本检查、rollback 状态、进度日志。

2. WebSocket/Admin 状态接口增强
   - 对照 ESP-IDF HTTP server/WebSocket examples。
   - 增加系统状态、WiFi 状态、nav 状态、最近错误摘要。

3. GPS parser 测试样例
   - 收集 GPRMC/GPGGA 正常、无 fix、checksum 错误、字段缺失样例。
   - 为 `driver_gps.c` 抽出可测试 parser 函数或增加 host-side 测试。

4. 工具审计日志
   - 对 tool_use 输入、输出长度、耗时、错误码记录结构化日志。
   - 高风险工具如 OTA、PWM、nav_goto 增加明确状态前置检查。

优先级：最高。

### 阶段 2：导航与传感器增强

目标：改善实车运行稳定性。

任务：

1. AHRS 算法实验
   - 移植或重写 Mahony/Madgwick 核心。
   - 增加 CLI/工具开关，用于对比当前 yaw。

2. IMU 校准流程增强
   - 参考 i2cdevlib。
   - 增加陀螺零偏、加速度计静态校准、磁力计 hard/soft iron 校准记录。

3. L2 FSM 参数观测
   - 增加避障原因、side score、PID 项、sensor age 的诊断输出。
   - 让 LLM 在异常停车时获得更完整的传感器诊断。

优先级：高。

### 阶段 3：组件化和硬件扩展

目标：为后续硬件迭代降低成本。

任务：

1. 调研 ESP-IoT-Solution 的 I2C/SPI bus abstraction。
2. 评估是否把传感器驱动拆成独立 components。
3. 如果新增显示屏或更多传感器，参考 nopnop2002 示例风格与 ESP-IoT-Solution 组件。
4. 对硬件变体引入 Kconfig 或更清晰的 JSON schema。

优先级：中。

### 阶段 4：ROS 2 / micro-ROS 实验

目标：为高级机器人能力预留通道。

任务：

1. 创建 micro-ROS 实验分支。
2. 发布 `nav_situation` topic。
3. 发布传感器 topic。
4. 订阅目标点 topic。
5. 验证内存、网络、任务时序影响。

优先级：中低，依赖是否需要 ROS 2 上位机能力。

### 阶段 5：语音和多模态扩展

目标：如果 MimiClaw 未来从文字/IM Agent 扩展为语音 Agent。

任务：

1. 评估 arduino-audio-tools 或 ESP-IoT-Solution 音频组件。
2. 验证 I2S 麦克风、扬声器、音频播放。
3. 与 LLM 交互链路做内存预算。

优先级：低。

## 4. 推荐近期落地清单

如果只选择 3 个最值得马上做的方向，建议：

1. **OTA 可靠性增强**
   - 价值最高，直接影响远程升级和现场恢复。
   - 参考 `espressif/esp-idf` OTA examples。

2. **AHRS 姿态融合实验**
   - 直接影响导航稳定性。
   - 参考 `adafruit/Adafruit_AHRS`，优先 Mahony/Madgwick。

3. **WebSocket/Admin 诊断接口增强**
   - 直接提升排错效率。
   - 参考 ESP-IDF HTTP/WebSocket examples。

## 5. 不建议近期做的事情

1. 不建议把当前项目迁移到 Arduino 框架。
2. 不建议直接引入完整 ROS/micro-ROS 到主线固件。
3. 不建议直接替换当前 L2 FSM。
4. 不建议为了使用外部库而引入大量 C++ 依赖。
5. 不建议在没有实机验证的情况下改动 PWM、避障和 OTA 主路径。

## 6. 下一步建议

建议下一轮从以下两个任务中选一个开始：

1. **OTA 增强方案设计与实现**
   - 风险可控，收益明确。
   - 主要影响 `main/ota/ota_manager.c` 和 `main/tools/tool_ota.c`。

2. **AHRS 姿态融合实验设计**
   - 对导航体验影响大。
   - 需要实机日志验证。
   - 主要影响 `main/drivers/driver_imu.c` 和 `main/nav/nav_situation.c`。
