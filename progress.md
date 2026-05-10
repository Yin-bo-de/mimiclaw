# MimiClaw 项目进展跟踪

## 项目概况
在 ESP32-S3 上实现自动导航系统，集成 HC-SR04 超声波避障 + MPU6050 IMU + NEO-6M GPS 导航。

## 分阶段开发计划

### Phase 1: HC-SR04 驱动 (已完成) ✅

**完成状态：** 100%
**完成时间：** 2026-05-10

**已实现的功能：**

1. **传感器配置管理**
   - 新增 `drivers/sensor_config.h` - 配置定义
   - 新增 `drivers/sensor_config.c` - 配置加载
   - 从 `/spiffs/config/sensors.json` 读取超声波传感器配置
   - 支持配置：TRIG/ECHO 引脚、最大量程、轮询间隙

2. **HC-SR04 驱动**
   - 新增 `drivers/driver_ultrasonic.h` - 驱动接口
   - 新增 `drivers/driver_ultrasonic.c` - 驱动实现
   - 使用 GPIO + 精确计时（esp_timer_get_time）实现高分辨率测量
   - 后台 FreeRTOS 任务（优先级 5，核心 1）轮询测量
   - 支持单通道单次测量和三通道连续测量
   - 实现距离计算（echo 脉冲宽度 / 58）

3. **工具层封装**
   - 新增 `tools/tool_sensors.h` - 传感器工具接口
   - 新增 `tools/tool_sensors.c` - 传感器工具实现
   - 新增 `ultrasonic_test` 工具（支持 CLI 调用）
   - 支持连续测试模式、自定义采样数和延迟

4. **CLI 命令**
   - 在 `cli/serial_cli.c` 中新增 `ultrasonic_test` 命令
   - 支持参数：
     - `-c <n>`：采样数量（默认 10）
     - `-d <ms>`：采样间隔（默认 1000ms）
   - 显示格式：L:xxcm F:xxcm R:xxcm [V:1,1,1] Age:xxxms

5. **构建系统更新**
   - 修改 `main/CMakeLists.txt`
     - 新增 `drivers/driver_ultrasonic.c`
     - 新增 `drivers/sensor_config.c`
     - 新增 `tools/tool_sensors.c`
     - 确保包含 esp_timer 依赖

6. **工具注册表更新**
   - 修改 `tools/tool_registry.c`
     - 新增 `#include "tools/tool_sensors.h"`
     - `MAX_TOOLS` 从 30 增加到 31
     - 在 tool_registry_init() 中调用 tool_sensors_init()
     - 注册 ultrasonic_test 工具

7. **配置文件**
   - 新增 `spiffs_data/config/sensors.json`
   - 包含默认的 HC-SR04 引脚配置
   - 可配置三个超声波传感器（左、前、右）的 TRIG/ECHO 引脚

**测试命令：**
```bash
# 读取当前目录
ultrasonic_test          # 默认 10 次采样，间隔 1 秒
ultrasonic_test -c 20    # 20 次采样
ultrasonic_test -d 500   # 500ms 间隔
ultrasonic_test -c 5 -d 300  # 5 次采样，300ms 间隔
```

**构建命令：**
```bash
cd /Users/yinbo/AI_Project/mimiclaw
idf.py build
idf.py -p /dev/cu.usbmodem21201 flash monitor
```

**引脚分配：**
- 左：TRIG=10，ECHO=12
- 前：TRIG=13，ECHO=14
- 右：TRIG=15，ECHO=16

### Phase 2: MPU6050 驱动 (已完成) ✅

**完成状态：** 100%
**完成时间：** 2026-05-10

**已实现的功能：**

1. **传感器配置管理**
   - 在 `drivers/sensor_config.h` 中新增 `imu_config_t` 结构体
   - 在 `drivers/sensor_config.c` 中新增 `imu_load_defaults()` 函数
   - 从 `/spiffs/config/sensors.json` 读取 IMU 配置
   - 支持配置：I2C 端口、SDA/SCL GPIO、地址、频率、采样率、gyro bias
   - 默认配置：I2C0, SDA=8, SCL=9, 0x68, 400kHz, 100Hz

2. **MPU6050 驱动**
   - 新增 `drivers/driver_imu.h` - 驱动接口
   - 新增 `drivers/driver_imu.c` - 驱动实现
   - 使用 ESP-IDF I2C 主设备驱动进行通信
   - 实现互补滤波计算 roll/pitch/yaw
   - 支持 gyro bias 校准（5秒静态校准）
   - 后台 FreeRTOS 任务（优先级 6，核心 1）轮询测量
   - 测量结果包含加速度(xyz)、陀螺仪(xyz)、姿态角

3. **工具层封装**
   - 在 `tools/tool_sensors.h` 中新增 `tool_imu_test_execute` 声明
   - 在 `tools/tool_sensors.c` 中实现 `tool_imu_test_execute` 函数
   - 新增 `imu_test` 工具（支持 CLI 调用）
   - 支持连续测试模式、自定义采样数和延迟

4. **CLI 命令**
   - 在 `cli/serial_cli.c` 中新增 `imu_test` 命令
   - 支持参数：
     - `-c <n>`：采样数量（默认 10）
     - `-d <ms>`：采样间隔（默认 1000ms）
   - 显示格式：Roll:xx° Pitch:xx° Yaw:xx° Gx:xx Gy:xx Gz:xx [V:1] Age:xxxms

5. **构建系统更新**
   - 修改 `main/CMakeLists.txt`
     - 新增 `drivers/driver_imu.c`
     - 添加 `esp_driver_i2c` 到 REQUIRES 依赖
     - 确保包含 I2C 驱动支持

6. **工具注册表更新**
   - 修改 `tools/tool_registry.c`
     - `MAX_TOOLS` 从 31 增加到 32
     - 在 tool_registry_init() 中注册 imu_test 工具

7. **配置文件**
   - 修改 `spiffs_data/config/sensors.json`
   - 新增 IMU 配置段
   - 可配置 I2C 引脚、地址、频率、采样率、gyro bias

**测试命令：**
```bash
# 默认 10 次采样，间隔 1 秒
imu_test

# 20 次采样
imu_test -c 20

# 500ms 间隔
imu_test -d 500

# 5 次采样，300ms 间隔
imu_test -c 5 -d 300
```

**构建命令：**
```bash
cd /Users/yinbo/AI_Project/mimiclaw
idf.py build
idf.py -p /dev/cu.usbmodem21201 flash monitor
```

**引脚分配：**
- I2C SDA: GPIO 8
- I2C SCL: GPIO 9

### Phase 3: NEO-6M GPS 驱动 (已完成) ✅

**完成状态：** 100%
**完成时间：** 2026-05-10

**已实现的功能：**

1. **传感器配置管理**
   - 在 `drivers/sensor_config.h` 中新增 `gps_config_t` 结构体
   - 在 `drivers/sensor_config.c` 中新增 `gps_load_defaults()` 函数
   - 从 `/spiffs/config/sensors.json` 读取 GPS 配置
   - 支持配置：UART 端口、RX/TX 引脚、波特率
   - 默认配置：UART1, RX=GPIO17, TX=GPIO18, 9600bps

2. **NEO-6M GPS 驱动**
   - 新增 `drivers/driver_gps.h` - 驱动接口
   - 新增 `drivers/driver_gps.c` - 驱动实现
   - 使用 ESP-IDF UART 驱动进行通信
   - 实现 NMEA 0183 GPRMC/GPGGA 句子解析（手写最小解析器）
   - 支持 GPS 定位数据获取（纬度、经度、高度、速度、航向、卫星数量）
   - 实现后台 FreeRTOS 任务（Core 0, 优先级 4）
   - 支持数据有效性检查和 stale 检测

3. **工具层封装**
   - 在 `tools/tool_sensors.h` 中新增 `tool_gps_test_execute()` 声明
   - 在 `tools/tool_sensors.c` 中实现 `tool_gps_test_execute()` 函数
   - 新增 `gps_test` 工具（支持 CLI 调用）
   - 支持连续测试模式、自定义采样数和延迟

4. **CLI 命令**
   - 在 `cli/serial_cli.c` 中新增 `gps_test` 命令
   - 支持参数：
     - `-c <n>`：采样数量（默认 10）
     - `-d <ms>`：采样间隔（默认 1000）
   - 显示格式：Lat/Lon/Alt/Speed/Course/Sats/V/Age

5. **构建系统更新**
   - 修改 `main/CMakeLists.txt`
     - 新增 `drivers/driver_gps.c`
     - 添加 `esp_driver_uart` 到 REQUIRES 依赖

6. **工具注册表更新**
   - 修改 `tools/tool_registry.c`
     - `MAX_TOOLS` 从 32 增加到 33
     - 在 `tool_registry_init()` 中注册 `gps_test` 工具

7. **配置文件更新**
   - 修改 `spiffs_data/config/sensors.json`
     - 新增 GPS 配置段：uart_port, rx_gpio, tx_gpio, baudrate

**测试命令：**
```bash
# 读取当前目录
gps_test              # 默认 10 次采样，间隔 1 秒
gps_test -c 20       # 20 次采样
gps_test -d 500      # 500ms 间隔
gps_test -c 5 -d 300 # 5 次采样，300ms 间隔
```

**构建命令：**
```bash
cd /Users/yinbo/AI_Project/mimiclaw
idf.py build
idf.py -p /dev/cu.usbmodem21201 flash monitor
```

**引脚分配：**
- GPS RX (GPS TX)：GPIO17
- GPS TX (GPS RX)：GPIO18
- UART 端口：UART1
- 波特率：9600

### Phase 4: 态势结构 + Waypoint 存储 + 传感器工具 (已完成) ✅

**完成状态：** 100%
**完成时间：** 2026-05-10

**已实现的功能：**

1. **导航态势结构 (`nav/nav_situation.{h,c}`)**
   - mutex 保护的共享态势结构 `nav_situation_t`
   - 包含超声波（3路）、IMU（roll/pitch/yaw/gz）、GPS（lat/lon/fix/sats/speed/course）、导航目标字段
   - 提供 init/get/update_distances/update_imu/update_gps/set_goal/clear_goal 接口
   - L1/L2 任务（后续 Phase）通过此结构消费传感器数据

2. **导航参数配置 (`nav/nav_config.{h,c}`)**
   - 从 `/spiffs/config/nav.json` 加载运行时参数
   - 覆盖 L1（emergency_stop_cm）、L2（速度、阈值、PID）、escalate（冷却、卡死判定）所有参数
   - 文件缺失时自动使用规格书默认值

3. **Waypoint 存储 (`nav/nav_waypoints.{h,c}`)**
   - CRUD 操作 `/spiffs/config/waypoints.json`
   - 支持命名存储（save）、按名查找（find）、删除（delete）、枚举（list）
   - 最大 32 条，重启后持久化保留
   - mutex 保护，写操作实时同步到 SPIFFS

4. **导航规划工具 (`nav/nav_planner.{h,c}`)**
   - Haversine 大圆距离计算（精度 <1m@100m）
   - 方位角计算（目标方向，0=北，顺时针）
   - 航向误差归一化（[-180°, +180°]，供 L2 PID 使用）

5. **LLM 传感器读取工具（扩展 `tool_sensors.{h,c}`）**
   - `read_distance`：返回三路超声波当前距离 + 有效性 + 数据新鲜度（JSON）
   - `read_imu`：返回 roll/pitch/yaw/gz_dps + 有效性（JSON）
   - `read_gps`：返回 lat/lon/fix/sats/speed_mps/course_deg（JSON）

6. **导航工具层 (`tools/tool_nav.{h,c}`)**
   - `nav_save_waypoint`：读当前 GPS，无 fix 时拒绝，成功写入 waypoints.json
   - `nav_list_waypoints`：返回所有 waypoint 列表（JSON）
   - `nav_delete_waypoint`：按名删除 waypoint
   - `nav_status`：返回当前导航状态（Phase 4 = IDLE）+ GPS + 距离 + 姿态 + 目标信息

7. **构建系统与注册**
   - `CMakeLists.txt`：新增 nav/ 目录 4 个 .c 文件 + tools/tool_nav.c
   - `mimi_config.h`：添加导航任务参数常量（L1/L2 栈/优先级/周期）+ 文件路径常量
   - `tool_registry.c`：MAX_TOOLS 33 → 40，注册 7 个新工具（3 传感器 + 4 导航）

8. **SPIFFS 配置文件**
   - 新增 `spiffs_data/config/nav.json`（规格书默认参数）
   - 新增 `spiffs_data/config/waypoints.json`（初始空列表）

**验收测试流程：**
```
# 通过 Telegram/飞书 / CLI 发送：
read_gps              # 读取当前 GPS 数据
nav_save_waypoint {"name":"测试点"}  # 保存当前位置（需 GPS fix）
nav_list_waypoints    # 列出所有 waypoint
# 重启设备后：
nav_list_waypoints    # 验证 waypoint 仍持久化
nav_delete_waypoint {"name":"测试点"}  # 删除
```

**引脚分配（延续前序 Phase 配置）：**
- 超声波：L(TRIG=10,ECHO=12) F(TRIG=13,ECHO=14) R(TRIG=15,ECHO=16)
- IMU I2C：SDA=8, SCL=9
- GPS UART1：RX=17, TX=18

**产出文件清单：**
- 新增：`main/nav/nav_situation.{h,c}` `nav_config.{h,c}` `nav_waypoints.{h,c}` `nav_planner.{h,c}`
- 新增：`main/tools/tool_nav.{h,c}`
- 新增：`spiffs_data/config/nav.json` `waypoints.json`
- 修改：`main/tools/tool_sensors.{h,c}` `tool_registry.c` `mimi_config.h` `CMakeLists.txt`

### Phase 5: L1 反射层 (已完成) ✅

**完成状态：** 100%
**完成时间：** 2026-05-10

**已实现的功能：**

1. **内部 RC 控制 API (`tools/tool_pwm.{h,c}`)**
   - 新增 `rc_nav_throttle(int pct)` - 绕过 JSON 直接设置油门（-100..100%）
   - 新增 `rc_nav_steer(int pct)` - 绕过 JSON 直接设置转向（-100..100%）
   - 复用现有 rc_config 及 LEDC PWM 通道，无需重新初始化
   - rc.json 未加载时返回 ESP_ERR_INVALID_STATE

2. **L1 反射层 (`nav/nav_l1_reflex.{h,c}`)**
   - 50 Hz FreeRTOS 任务（优先级 7，核心 1，栈 3KB）
   - 每 20ms 读取 nav_situation_t，计算三路超声波最小有效距离
   - 传感器数据 stale > 500ms 则视为无效（按无障碍处理，避免误停）
   - `min_d < emergency_stop_cm`（默认 20cm）时：强制 throttle=0 并置 blocked=true
   - 障碍清除时：置 blocked=false，高层任务自动恢复控制
   - 暴露 `nav_l1_is_blocked()` 供 L2 / dummy drive 查询

3. **导航控制器 (`nav/nav_controller.{h,c}`)**
   - 顶层状态机：IDLE / RUNNING / PAUSED
   - `nav_controller_init()` - 初始化 L1（不启动任务）
   - `nav_controller_start()` - 启动 L1 反射任务
   - `nav_controller_dummy_drive_start(speed_pct)` - Phase 5 测试钩子
     - 以固定速度直行，每 100ms 检查 L1 blocked 状态
     - L1（prio=7）抢占 dummy（prio=4），保证 < 20ms 内刹停
   - `nav_controller_dummy_drive_stop()` - 安全停止测试

4. **CLI 测试命令 (`cli/serial_cli.c`)**
   - 新增 `l1_test` 命令
   - `l1_test [-s <speed_pct>]`：以指定速度（默认 20%）启动 dummy drive
   - `l1_test -x`：停止 dummy drive
   - 提示用户障碍检测逻辑和操作指引

5. **构建系统与启动序列**
   - `CMakeLists.txt`：新增 `nav/nav_l1_reflex.c` + `nav/nav_controller.c`
   - `mimi.c`：
     - `nav_controller_init()` 在 `tool_registry_init()` 之后调用（WiFi 无关）
     - `nav_controller_start()` 在 `ws_server_start()` 之后调用（WiFi 就绪后）

**验收测试方法：**
```bash
# 在串口 CLI 中执行：
l1_test -s 20       # 以 20% 速度启动 dummy drive
# 将手或纸板移近任意超声波传感器 < 20cm
# 观察：小车立即停止（< 50ms），移开后自动恢复前进
l1_test -x          # 停止测试
```

**引脚分配（继承前序 Phase）：**
- 超声波：L(TRIG=10,ECHO=12) F(TRIG=13,ECHO=14) R(TRIG=15,ECHO=16)
- 电机 ESC：GPIO 21（throttle），舵机：GPIO 11（steer）

**产出文件清单：**
- 新增：`main/nav/nav_l1_reflex.{h,c}` `nav_controller.{h,c}`
- 修改：`main/tools/tool_pwm.{h,c}` `main/mimi.c` `main/CMakeLists.txt` `main/cli/serial_cli.c`

### Phase 6: L2 战术层 (已完成) ✅

**完成状态：** 100%
**完成时间：** 2026-05-10

**已实现的功能：**

1. **避障历史记忆 (`nav/nav_memory.{h,c}`)**
   - 8 条环形缓冲 `avoid_record_t`（lat/lon/side/succeeded/d_front/ts_us/duration_ms）
   - `nav_memory_record_attempt()` — 进入 AVOID 时记录
   - `nav_memory_mark_last_result()` — 离开 AVOID 时标记成功/失败
   - `nav_memory_penalty_for_side()` — Haversine 10m 范围内 failed/total 评分 [0.0, 1.0]

2. **L2 战术层 FSM (`nav/nav_l2_fsm.{h,c}`)**
   - 8 状态：CRUISE / AVOID_LEFT / AVOID_RIGHT / REVERSE / REPLAN / PAUSED / ARRIVED / FAULT
   - 10 Hz FreeRTOS 任务（优先级 5，核心 1，栈 6KB）
   - **CRUISE**：Haversine 到达判断 + 航向 PID（Kp×heading_error，clamp）+ 障碍触发评分
   - **AVOID_LEFT/RIGHT**：满舵转向 + 前方 < emergency_reverse → REVERSE；clear → CRUISE；超时 → REPLAN
   - **REVERSE**：直行倒车 + 超时 → REPLAN
   - **REPLAN**：throttle=0，-100→+100→-100 扫描 replan_ms；发现 clear 侧 → AVOID_*；扫完仍堵 replan_count>3 → FAULT
   - **PAUSED**：throttle=0 保持；resume 回 prev_state
   - **ARRIVED / FAULT**：throttle=0，steer=0 终态（Phase 7 加 escalate）
   - 评分函数：clearance_term + heading_term - memory_penalty
   - 传感器 stale > 2s → FAULT，replan_count > 3 → FAULT（NO_PATH，Phase 7 escalate）
   - 命令通道：volatile l2_cmd_t（PAUSE / RESUME / ABORT / NEW_GOAL）

3. **导航控制器扩展 (`nav/nav_controller.{h,c}`)**
   - `nav_controller_set_goal(lat, lon, name, speed_pct)` — 设置 situation goal + 启动 L2
   - `nav_controller_set_goal_by_waypoint(name, speed_pct)` — 按名查 waypoint + 调上述
   - `nav_controller_pause()` / `nav_controller_resume()` / `nav_controller_abort()`
   - `nav_controller_get_l2_state_name()` — 返回 L2 FSM 状态字符串
   - `nav_controller_init()` 同时初始化 L1 + L2

4. **工具层补全 (`tools/tool_nav.{h,c}`)**
   - `nav_goto` — 设置绝对 GPS 目标启动导航
   - `nav_goto_waypoint` — 按名导航至 waypoint
   - `nav_pause` / `nav_resume` — 暂停/恢复（保留 FSM 上下文）
   - `nav_abort` — 终止导航回 IDLE
   - `nav_manual_step` — 临时接管 ≤1000ms，L2 自动恢复；L1 守护仍生效
   - `nav_status` — 更新返回 L2 FSM state + avoid/replan 计数 + bearing_deg

5. **构建系统与注册**
   - `CMakeLists.txt`：新增 `nav/nav_memory.c` + `nav/nav_l2_fsm.c`
   - `tool_registry.c`：MAX_TOOLS 40 → 48，注册 6 个新工具
   - 编译结果：零警告零错误，固件 1.3 MB（Flash 剩余 37%）

**引脚分配（继承前序 Phase）：**
- 超声波：L(TRIG=10,ECHO=12) F(TRIG=13,ECHO=14) R(TRIG=15,ECHO=16)
- IMU I2C：SDA=8, SCL=9
- GPS UART1：RX=17, TX=18
- 电机 ESC：GPIO 21，舵机：GPIO 11

**验收测试方法（分级）：**
```
# 验收 1：空旷场地 20m 直达
nav_goto_waypoint {"name":"目标点"}  # 预先 nav_save_waypoint 存好
# 观察：小车直行，到达后 nav_status 返回 l2_state=ARRIVED，最终距离 <3m

# 验收 2：单障碍绕行
# 在 5m 处放置障碍物，执行 nav_goto
# 观察：CRUISE → AVOID_LEFT or AVOID_RIGHT → 绕开 → CRUISE → ARRIVED

# 验收 3：墙角脱出
# 靠近墙角，执行 nav_goto
# 观察：AVOID → REVERSE → REPLAN → 找到 clear 侧 → 脱出无碰撞

# 临时接管测试
nav_pause
nav_manual_step {"steer_pct":-100,"throttle_pct":-25,"hold_ms":800}
nav_resume  # 自动恢复
```

**产出文件清单：**
- 新增：`main/nav/nav_memory.{h,c}` `nav_l2_fsm.{h,c}`
- 修改：`main/nav/nav_controller.{h,c}` `main/tools/tool_nav.{h,c}` `main/tools/tool_registry.c` `main/CMakeLists.txt`

### Phase 7: escalate 事件管线 (已完成) ✅

**完成状态：** 100%
**完成时间：** 2026-05-10

**已实现的功能：**

1. **escalate 核心模块 (`nav/nav_escalate.{h,c}`)**
   - 定义 `escalate_kind_t`：ESC_STUCK / ESC_OSCILLATING / ESC_NO_PATH / ESC_LOST / ESC_GOAL_UNREACHABLE / ESC_ARRIVED / ESC_ABORTED
   - `nav_escalate_init()` — 初始化冷却计时器和内部状态
   - `nav_escalate_set_origin(channel, chat_id)` — 记录本次导航会话的来源
   - 终态事件（无冷却）：`nav_escalate_arrived()` / `nav_escalate_aborted()`
   - 异常事件（60s 冷却）：`nav_escalate_no_path()`
   - 周期检测：`nav_escalate_run_periodic()` — STUCK / OSCILLATING / LOST / GOAL_UNREACHABLE
   - 振荡追踪：`nav_escalate_notify_avoid_entry(side)` — 记录每次 AVOID 方向进入
   - 所有事件通过 `message_bus_push_inbound()` 投递到 agent，携带 origin + JSON payload + hint

2. **Origin 透传机制**
   - `tool_registry.h` — 新增 `tool_msg_origin_t` 结构体和 `set_origin/get_origin` 函数
   - `tool_registry.c` — 全局 `s_current_origin` 存储（agent_loop 单任务，无竞态）
   - `agent_loop.c` — 每条消息处理开始时调用 `tool_registry_set_origin(msg.channel, msg.chat_id)`
   - `tool_nav.c` — `nav_goto` / `nav_goto_waypoint` 读取 origin 后调用 `nav_escalate_set_origin()`，确保 escalate 事件精确回到发起人

3. **L2 FSM 接入 escalate**
   - `nav_l2_fsm.c` — 新增 `s_trip_start_us` 记录行程开始时间（用于 ARRIVED 事件的用时计算）
   - ARRIVED：在 `fsm_cruise` 中到达判断后调用 `nav_escalate_arrived(sit, duration_s, dist, avoid_count)`
   - NO_PATH：在 `enter_state` 中 replan_count > 3 时调用 `nav_escalate_no_path(&tmp_sit)`
   - ABORTED（user_abort）：在 `L2_CMD_ABORT` 处理中计算剩余距离后调用 `nav_escalate_aborted()`
   - ABORTED（sensors_lost）：在传感器 stale 检测 FAULT 时调用 `nav_escalate_aborted(&sit, "sensors_lost", 0.0)`
   - 振荡通知：`enter_state` 中进入 AVOID_LEFT/RIGHT 时调用 `nav_escalate_notify_avoid_entry(side)`
   - 周期检测：每 L2 tick 末尾（活跃导航状态时）调用 `nav_escalate_run_periodic()`

4. **Context Builder 注入 NAV_PLAYBOOK**
   - `context_builder.c` — 在系统 prompt 末尾 `append_file(MIMI_NAV_PLAYBOOK_FILE)`
   - `mimi_config.h` — 新增 `MIMI_NAV_PLAYBOOK_FILE` 路径常量
   - `spiffs_data/config/NAV_PLAYBOOK.md` — 7 种事件类型的 LLM 决策手册（自然语言回复模板 + 工具调用策略）

5. **构建系统更新**
   - `CMakeLists.txt` — 新增 `nav/nav_escalate.c`
   - 编译结果：零警告零错误，固件 1.33 MB（Flash 剩余 37%）

**验收测试方法：**
```
# 测试 1：正常到达
nav_goto_waypoint {"name":"测试点"}
# 小车导航到达 → Telegram 自动收到 "✅ 已到达测试点..." 类回复

# 测试 2：主动取消
# 导航行驶中，在 Telegram 说 "停下" → LLM 调 nav_abort
# → Telegram 收到 "已停止..." 回复

# 测试 3：异常事件（STUCK）
# 故意阻止小车移动 > 10 秒 → STUCK 事件触发 → agent 介入
# → 60s 冷却内再次触发不重复上报

# 测试 4：Origin 验证
# Telegram 发起导航 → Telegram 收到 escalate 事件（不会发到 Feishu）
```

**引脚分配（继承前序 Phase）：**
- 超声波：L(TRIG=10,ECHO=12) F(TRIG=13,ECHO=14) R(TRIG=15,ECHO=16)
- IMU I2C：SDA=8, SCL=9
- GPS UART1：RX=17, TX=18
- 电机 ESC：GPIO 21，舵机：GPIO 11

**产出文件清单：**
- 新增：`main/nav/nav_escalate.{h,c}` `spiffs_data/config/NAV_PLAYBOOK.md`
- 修改：`main/tools/tool_registry.{h,c}` `main/agent/agent_loop.c` `main/tools/tool_nav.c`
         `main/nav/nav_l2_fsm.c` `main/mimi_config.h` `main/agent/context_builder.c` `main/CMakeLists.txt`

### Phase 8: 规则引擎导航传感器触发扩展 (已完成) ✅

**完成状态：** 100%
**完成时间：** 2026-05-10

**已实现的功能：**

1. **规则引擎扩展 (`rule_engine/rule_engine.{h,c}`)**
   - 新增 3 种触发类型：`RULE_TRIGGER_ULTRASONIC_DISTANCE` / `RULE_TRIGGER_IMU_TILT` / `RULE_TRIGGER_GPS_DISTANCE_TO`
   - 扩展 `rule_trigger_t`：新增 `channel`（int）、`lat`/`lon`（double）字段
   - 新增 3 个 eval 函数，直接读取 `nav_situation_t` 快照：
     - `eval_trigger_ultrasonic_distance(channel)` — 返回 distances_cm[channel]（cm）；channel 越界或 stale > 500ms 无效
     - `eval_trigger_imu_tilt(channel)` — 返回 `|roll|`（ch=0）或 `|pitch|`（ch=1）取整（度）；stale > 500ms 无效
     - `eval_trigger_gps_distance(lat, lon)` — 返回 haversine 距离取整（米）；GPS 无 fix 或 stale > 5s 无效
   - 更新 parse/load/save：新字段完整序列化/反序列化到规则 JSON 文件
   - `parse_trigger_type` 新增未知字符串 LOGW 警告

2. **工具层扩展 (`tools/tool_rule.c` + `tool_registry.c`)**
   - `parse_trigger_type` 识别 3 个新字符串
   - `tool_rule_add_execute`：解析 `channel`/`lat`/`lon`；各类型参数验证（channel 越界、GPS 字段存在性）
   - `tool_rule_list_execute`：按触发类型显示 `ch=N` 或 `lat/lon` 详情；条件 op 覆盖所有 10 种
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

---

## Phase 2 测试与修复 (2026-05-10)

**测试场景：** IMU 静止 60s yaw 漂移测试
**测试命令：** `imu_test -c 60 -d 1000`
**测试数据：**
- 静态 Gyro z ≈ 0.5°/s（未校准）
- Yaw 从 5764.3° 漂移到 5796.8°（60s 漂移 32.5°）
- 远超目标 <15°/60s（实际 0.5°/s × 60s = 30°）

**根因分析：**
1. `driver_imu_start()` 从未调用 `driver_imu_calibrate_gyro()`，开机永远使用 `sensors.json` 中的 `[0,0,0]` bias
2. `sensor_config.c` 只有 load 逻辑，没有 save 逻辑，校准结果重启丢失
3. `imu_update()` 中 yaw 未做 `fmod` 归一化，积分无限累积到 5764°+

**修复内容：**

| 文件 | 修改 |
|------|------|
| `sensor_config.h` | 新增 `sensor_config_save_imu_bias()` 声明 |
| `sensor_config.c` | 实现 `sensor_config_save_imu_bias()`：读取 sensors.json → 修改 `imu.gyro_bias_dps` → 重写文件 |
| `driver_imu.c` | `imu_update()` 中 yaw 做 `while (s_yaw >= 360) s_yaw -= 360` 归一化 |
| `driver_imu.c` | `driver_imu_start()` 启动时自动检测 bias 是否全零，若是则自动校准 5s 并持久化到 sensors.json |
| `serial_cli.c` | 新增 `imu_calibrate` CLI 命令，支持手动重新校准 |

**预期修复后验证：**
```bash
imu_test -c 60 -d 1000
# 预期：Gyro z 静态 ≈ 0.00°/s，Yaw 在 0~360° 范围，60s 漂移 <5°
```

**产出文件清单：**
- 修改：`main/drivers/sensor_config.h` `main/drivers/sensor_config.c` `main/drivers/driver_imu.c` `main/cli/serial_cli.c`
- 新增：`test_plan.md`（各 Phase 完整测试指南含 CLI 命令速查表）

---

## Phase 4 集成断点修复 (2026-05-10)

**测试场景：** `tool_exec nav_status {}` 返回 IMU 全 0
**测试数据：**
```json
{"state":"IDLE","l2_state":"IDLE","has_goal":false,
 "gps":{"lat":0,"lon":0,"fix":false,"sats":0},
 "distances":{"left_cm":400,"front_cm":400,"right_cm":400},
 "imu":{"yaw":0,"roll":0,"pitch":0}}
```

**根因分析：**
`nav_situation_t` 是 L1/L2/工具层的单一态势源，但三个传感器驱动（imu/ultrasonic/gps）**从未调用 `nav_situation_update_*()`**。数据流在驱动层和导航层之间完全断裂，导致：
1. `nav_status` 永远读到初始化默认值（imu=0, distances=400）
2. L1 反射层读到的 distances 永远是 400cm，紧急停车永远不会触发
3. L2 FSM 读到的 IMU/GPS 全为零，导航决策无法工作

**修复内容：**

| 文件 | 修改 |
|------|------|
| `driver_imu.c` | `#include "nav/nav_situation.h"`；`imu_update()` 末尾调用 `nav_situation_update_imu(s_roll, s_pitch, s_yaw, gz)` |
| `driver_ultrasonic.c` | `#include "nav/nav_situation.h"`；每轮轮询后 `driver_ultrasonic_get_all()` + `nav_situation_update_distances()` |
| `driver_gps.c` | `#include "nav/nav_situation.h"`；`parse_gprmc()` 和 `parse_gpgga()` 后各调用 `nav_situation_update_gps()` |

**验证方法：**
```bash
tool_exec nav_status {}
# 预期：imu.yaw/roll/pitch 与 imu_test 读数一致（非 0）
# distances 反映真实障碍物距离（非固定 400）
```

**产出文件清单：**
- 修改：`main/drivers/driver_imu.c` `main/drivers/driver_ultrasonic.c` `main/drivers/driver_gps.c`

---

## 启动时序竞态修复 (2026-05-10)

**测试场景：** 烧录后启动立即 panic
**回溯：**
```
assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))
Backtrace: ... nav_situation_update_distances ... ultrasonic_task
```

**根因分析：**
启动序列中 `tool_registry_init()` (L144) 在 `nav_controller_init()` (L145) 之前执行。
`tool_registry_init()` → `tool_sensors_init()` → `driver_ultrasonic_start()` 创建 `ultrasonic_task`。
任务一创建立即被调度运行，第一轮轮询完调用 `nav_situation_update_distances()`，
此时 `nav_situation_init()` 尚未执行，`s_mutex` 为 NULL，触发 FreeRTOS assert。

**修复内容：**
在 `nav_situation.c` 所有 update/set/clear 函数入口统一增加 `if (!s_mutex) return;` guard：
- `nav_situation_update_distances()`
- `nav_situation_update_imu()`
- `nav_situation_update_gps()`
- `nav_situation_set_goal()`
- `nav_situation_clear_goal()`

**产出文件清单：**
- 修改：`main/nav/nav_situation.c`
