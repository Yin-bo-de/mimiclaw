# MimiClaw 自动导航系统 — 实施规划

> 目标：在 ESP32-S3 + ESP-IDF v5.5.2 的 MimiClaw 项目上，集成 NEO-6M (GPS) + 3×HC-SR04 (超声波) + MPU6050 (IMU)，实现"小车从 A 自动行驶到 B、自主避障"的能力。

---

## Context（为什么做这件事）

**当前状态**：项目仅有 RC 遥控控制（`rc_steer` / `rc_throttle`，见 `main/tools/tool_pwm.c:327-419`）和数字 GPIO 读写。无任何传感器驱动、无定位、无导航算法、无避障逻辑。

**用户需求**：小车从 A 点自动行驶到 B 点，遇障自主决策（左转 / 右转 / 后退）。手头硬件：NEO-6M、HC-SR04 ×3、MPU6050。

**核心约束与设计决策**（与用户确认）：
1. **场景**：纯室外 GPS 主导（不做室内航位推算与双模切换，简化设计）
2. **控制架构**：三层混合
   - L1 反射层（C 代码 50 Hz）：紧急刹车，最近距离 < 20cm 强制停车
   - L2 战术层（C 代码 10 Hz）：FSM + 记忆，自主避障决策
   - L3 战略层（LLM 按需介入）：仅在 escalate 事件触发时由 Claude 处理
3. **L2 状态机**：CRUISE / AVOID_LEFT / AVOID_RIGHT / REVERSE / REPLAN 五状态，带 8 条避障历史 + "哪侧更空旷 + 目标方向加权 + 上次失败侧降权"评分
4. **超声波**：3 个，左前(-30°) / 正前(0°) / 右前(+30°)
5. **escalate 事件**：7 类（成功事件：ARRIVED；失败/异常事件：STUCK / OSCILLATING / NO_PATH / LOST / GOAL_UNREACHABLE / ABORTED），异常类每类 60 秒冷却，成功/取消类立即触发不冷却。LLM 收到后用自然语言通知用户（到达 / 卡住 / 已取消）。
6. **Waypoint 命名机制**：用户遥控车到目标点 → `nav_save_waypoint("快递柜")` 自动存 GPS 经纬度，之后 `nav_goto_waypoint("快递柜")` 自动取出导航；用户全程不接触经纬度数字

**预期成果**：用户在 Telegram 说"小车去快递柜"，小车自主导航到目标点（误差 ≤ 3m），中途遇到障碍能自主绕开；**到达后主动回复用户**（如"已到达快递柜，用时 1 分 23 秒，最终距离 2.1 米"）；遇到无法解决的复杂情境上报 LLM 介入或终止并告知原因。

---

## 1. 整体架构

```
LLM (Claude API)                            ◄── escalate 事件 (60s 冷却)
   │ tool_call: nav_goto_waypoint("快递柜")
   ▼
┌─────────────────────────────────────────────────┐
│  nav_controller (顶层状态: IDLE/RUNNING/PAUSED) │
└──────────────┬──────────────────────────────────┘
               │ 设置目标
               ▼
┌──────────────────────────────────┐    ┌───────────────────┐
│  L2 nav_l2_task (10 Hz)          │◄──►│ nav_situation     │
│  FSM + 记忆 + 评分               │    │ (mutex 保护的     │
│  + escalate 检测                 │    │  共享态势结构)     │
└──────────────┬───────────────────┘    └───▲───────────────┘
               │ steer / throttle 指令         │ 写入
               ▼                               │
┌──────────────────────────────────┐    ┌─────┴─────┐ ┌──────────┐ ┌──────────┐
│  L1 nav_l1_task (50 Hz)          │    │ driver_   │ │ driver_  │ │ driver_  │
│  紧急刹车守护                    │    │ ultrasonic│ │ imu      │ │ gps      │
└──────────────┬───────────────────┘    │ (5.5 Hz)  │ │ (100 Hz) │ │ (UART)   │
               │ rc_steer / rc_throttle └───┬───────┘ └────┬─────┘ └────┬─────┘
               ▼                            │              │           │
         [伺服 + 电机]               [3×HC-SR04]      [MPU6050]    [NEO-6M]
                                       (RMT)         (I2C 0x68)    (UART1)
```

**关键设计原则**：
- **单一态势源**：所有传感器只写 `nav_situation_t`，所有消费者（L1/L2/工具）只读它。
- **优先级保证**：L1 prio=7（最高），L2=5，agent_loop=6（已有）。L1 在 Core 1 上能抢占 L2。
- **escalate 不阻塞 L2**：LLM 思考期间 L2 持续运行，需要时由 LLM 调用 `nav_pause()` 暂停。

---

## 2. 模块划分与新增文件

新增三个目录：`main/drivers/` / `main/nav/` / `main/tools/`(扩展)。

### 2.1 驱动层 `main/drivers/`

| 文件 | 职责 | 估算 LOC |
|------|------|---------|
| `driver_imu.{h,c}` | MPU6050 I2C 驱动 + 互补滤波（roll/pitch/yaw） | 350 |
| `driver_gps.{h,c}` | NEO-6M UART + NMEA GPRMC/GPGGA 解析（手写最小解析器，不引第三方库） | 400 |
| `driver_ultrasonic.{h,c}` | 3×HC-SR04 RMT 驱动 + 严格 60ms 轮询防串扰 | 380 |
| `sensor_config.{h,c}` | 加载 `/spiffs/config/sensors.json`（参考 `tool_pwm.c:178-232` 模式） | 180 |

### 2.2 导航引擎层 `main/nav/`

| 文件 | 职责 | 估算 LOC |
|------|------|---------|
| `nav_situation.{h,c}` | 共享态势结构 + mutex（单一数据源） | 220 |
| `nav_l1_reflex.{h,c}` | 50 Hz 反射层，min_d<20cm 强制停车 | 200 |
| `nav_l2_fsm.{h,c}` | 10 Hz FSM + 评分 + 航向 PID | 700 |
| `nav_memory.{h,c}` | 8 条避障历史环形缓冲 + 加权评分 | 150 |
| `nav_escalate.{h,c}` | 5 类 escalate 检测器 + 冷却 + 上报消息总线 | 280 |
| `nav_waypoints.{h,c}` | `/spiffs/config/waypoints.json` 增删查改 | 240 |
| `nav_planner.{h,c}` | Haversine 距离、目标方位角、航向归一化 | 130 |
| `nav_config.{h,c}` | 加载 `/spiffs/config/nav.json` | 160 |
| `nav_controller.{h,c}` | 顶层编排（启动/暂停/目标设置） | 250 |

### 2.3 工具层 `main/tools/`（扩展）

| 文件 | 职责 | 估算 LOC |
|------|------|---------|
| `tool_sensors.{h,c}` | LLM 工具：`read_distance` / `read_imu` / `read_gps` | 230 |
| `tool_nav.{h,c}` | LLM 工具：`nav_goto` / `nav_goto_waypoint` / `nav_save_waypoint` / `nav_list_waypoints` / `nav_delete_waypoint` / `nav_pause` / `nav_resume` / `nav_abort` / `nav_status` / `nav_manual_step`（10 个） | 600 |

### 2.4 修改的现有文件

| 文件 | 修改 |
|------|------|
| `main/CMakeLists.txt` | SRCS 添加新文件；REQUIRES 添加 `esp_driver_i2c` `esp_driver_uart` `esp_driver_rmt` |
| `main/mimi_config.h` | 在 `:151` 后插入 ~30 个导航相关常量（栈/优先级/核心/文件路径） |
| `main/tools/tool_registry.c:19` | `MAX_TOOLS` 从 30 → 48 |
| `main/tools/tool_registry.c:11` | 新增 `#include` |
| `main/tools/tool_registry.c:407` 前 | 调用 `tool_sensors_init()` + `tool_nav_init()` 并 `register_tool` 13 个新工具 |
| `main/rule_engine/rule_engine.h:8-17` | 新增 trigger 枚举 `RULE_TRIGGER_ULTRASONIC_DISTANCE` / `RULE_TRIGGER_IMU_TILT` / `RULE_TRIGGER_GPS_DISTANCE_TO`；扩展 `rule_trigger_t` 加 `channel` / `lat` / `lon` 字段 |
| `main/rule_engine/rule_engine.c:75-243` | 新增三个 eval 函数 + switch dispatch |
| `main/rule_engine/rule_engine.c:303-309` | `parse_trigger_type` 加新字符串 |
| `main/tools/tool_rule.c:14-138` | rule_add JSON Schema 与解析镜像扩展 |
| `main/mimi.c:147` `:195` | `nav_controller_init()` + `nav_controller_start()` |
| `spiffs_data/config/` | 新增 `sensors.json` / `nav.json` / `waypoints.json` |

**总量**：新增 ~4500 LOC，修改 ~250 LOC。

---

## 3. 硬件外设技术选型（已论证）

### 3.1 HC-SR04 → 用 RMT（不用 GPIO 计时）

回波脉宽编码距离（58 µs/cm，最大 23 ms@4m）。RMT 用 100ns 硬件时间戳 + DMA 缓冲，零 CPU 占用，3 路 RX 通道刚好对应 3 个传感器（ESP32-S3 共 4 RX + 4 TX 通道）。GPIO 轮询会因高优先级任务抢占造成 jitter，pulse_cnt 只数脉冲数不测时长——都不合适。

### 3.2 MPU6050 → 互补滤波（不用 DMP/Madgwick）

6 轴无磁力计，yaw 必然漂移；DMP/Madgwick 同样漂。**关键技巧：用 GPS course 在车速 > 0.5 m/s 时校正 yaw**，这样漂移被 GPS 兜底。互补滤波 ~30 行 C 代码，<50 µs/帧；DMP 要 3KB Invensense blob + FIFO 状态机，得不偿失。

```c
roll  = 0.98*(roll  + gx*dt) + 0.02*atan2(ay, az);
pitch = 0.98*(pitch + gy*dt) + 0.02*atan2(-ax, sqrt(ay*ay+az*az));
yaw   = yaw + gz*dt;  /* GPS course 在 fix && speed>0.5 时覆盖 */
```

开机做 5 秒静止陀螺 bias 校准，存 `sensors.json`。

### 3.3 NEO-6M → UART_NUM_1 + 手写 NMEA 解析

UART 端口分配：`UART_NUM_0` 已被 console/serial_cli 占用，`UART_NUM_1` 给 GPS。手写最小 NMEA 解析器（只解 `$GPRMC` + `$GPGGA`、校验 checksum、行缓冲）— 不引 minmea/libnmea，省 flash。

### 3.4 三个 HC-SR04 → 严格 60ms 轮询（防串扰）

并发触发会让 A 的回波被 B 收到造成假数据。轮询节奏：每个传感器 10µs trig → 等 30ms 超时 → 30ms 静默 → 切下一个。3 路一轮 180ms（每路 5.5 Hz）。**L1 反射层不依赖即时新鲜度**：取三路最新读数中的 min，单读数 stale > 500ms 才视为无效（按最大量程处理避免误停）。

---

## 4. GPIO / 外设引脚分配（已核对避免冲突）

**已占用**（实测 `spiffs_data/config/rc.json:2,7`）：
- GPIO 11：steering 舵机
- GPIO 21：throttle ESC

**ESP32-S3 系统保留**（`gpio_policy.c:69`）：GPIO 19、20（USB Serial/JTAG）

**允许池**（`gpio_policy.h:9`）：`1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,46`

> 注意：ESP32-S3 上 GPIO 6-11 **没有被 block**（`gpio_policy.c:91-93`，因为 S3 模组 flash 走专用引脚），但 GPIO 11 已被 steering 占用。

**新增分配方案**（写入 `sensors.json`）：

| 用途 | GPIO | 备注 |
|------|------|------|
| MPU6050 SDA (I2C0) | 8 | ESP32-S3 默认 I2C 引脚 |
| MPU6050 SCL (I2C0) | 9 | |
| NEO-6M GPS RX (UART1) | 17 | 接 GPS TX |
| NEO-6M GPS TX (UART1) | 18 | 接 GPS RX |
| HC-SR04 左 -30° TRIG | 10 | |
| HC-SR04 左 -30° ECHO | 12 | |
| HC-SR04 前 0° TRIG | 13 | |
| HC-SR04 前 0° ECHO | 14 | |
| HC-SR04 右 +30° TRIG | 15 | |
| HC-SR04 右 +30° ECHO | 16 | |
| **预留扩展** | 1,2,3,4,5,6,7,38,46 | 编码器 / 显示屏 / 灯效 |

---

## 5. 任务架构（FreeRTOS）

新增到 `main/mimi_config.h:151` 后：

```c
/* Navigation tasks */
#define MIMI_NAV_DRIVER_GPS_STACK     (4 * 1024)
#define MIMI_NAV_DRIVER_GPS_PRIO      4
#define MIMI_NAV_DRIVER_GPS_CORE      0   /* I/O 与 channels 同核 */

#define MIMI_NAV_DRIVER_IMU_STACK     (3 * 1024)
#define MIMI_NAV_DRIVER_IMU_PRIO      6
#define MIMI_NAV_DRIVER_IMU_CORE      1

#define MIMI_NAV_DRIVER_US_STACK      (3 * 1024)
#define MIMI_NAV_DRIVER_US_PRIO       5
#define MIMI_NAV_DRIVER_US_CORE       1

#define MIMI_NAV_L1_STACK             (3 * 1024)
#define MIMI_NAV_L1_PRIO              7   /* 最高，必须抢占 L2 与 agent */
#define MIMI_NAV_L1_CORE              1
#define MIMI_NAV_L1_PERIOD_MS         20  /* 50 Hz */

#define MIMI_NAV_L2_STACK             (6 * 1024)
#define MIMI_NAV_L2_PRIO              5
#define MIMI_NAV_L2_CORE              1
#define MIMI_NAV_L2_PERIOD_MS         100 /* 10 Hz */

#define MIMI_NAV_ESCALATE_COOLDOWN_S  60

/* Config files */
#define MIMI_NAV_SENSORS_FILE     MIMI_SPIFFS_CONFIG_DIR "/sensors.json"
#define MIMI_NAV_PARAMS_FILE      MIMI_SPIFFS_CONFIG_DIR "/nav.json"
#define MIMI_NAV_WAYPOINTS_FILE   MIMI_SPIFFS_CONFIG_DIR "/waypoints.json"
```

**任务总览**：

| 任务 | 周期 | 栈 | 优先级 | 核心 | 备注 |
|------|------|------|--------|------|------|
| `driver_gps_task` | UART 阻塞 | 4KB | 4 | 0 | 与 telegram/feishu 同核（已有 channels 在 Core 0） |
| `driver_imu_task` | 10ms (100Hz) | 3KB | 6 | 1 | 紧循环，不能被网络任务饿死 |
| `driver_ultrasonic_task` | 自调度 60ms×3 | 3KB | 5 | 1 | RMT 驱动，大部分时间睡眠 |
| `nav_l1_task` | 20ms (50Hz) | 3KB | **7** | 1 | 最高优先级保证及时刹车 |
| `nav_l2_task` | 100ms (10Hz) | 6KB | 5 | 1 | FSM + 评分 + 历史 + escalate |
| `agent_loop` (已有) | event-driven | 24KB | 6 | 1 | 99% 阻塞在消息总线 |

**数据共享方式**：
- 传感器 → `nav_situation_t`：用 `SemaphoreHandle_t` mutex，hold time < 5µs，160 lock/s ≈ 0.08% CPU。**不用 queue**（L1/L2 要最新值不要历史）。
- escalate → agent：用 `xQueueCreate(8, sizeof(escalate_event_t))` 异步队列，由 `nav_escalate_task` 消费后调用 `message_bus_push_inbound()`（复用 `heartbeat.c:78-105` 模式）。

---

## 6. L2 状态机详细设计

### 6.1 状态转移图

```
                  ┌────────────────────┐
                  │       PAUSED       │◄──── nav_pause()
                  └────────┬───────────┘
                           │ resume
                           ▼
       ┌────────────► CRUISE ◄──────────────────┐
       │           (steer→bearing,               │
       │            throttle=CRUISE_SPEED_PCT)   │
       │                │                        │
       │                │ min_d < AVOID_TRIGGER  │
       │                ▼                        │
       │     score_sides() → AVOID_LEFT or _RIGHT│
       │           │                             │
       │           │ d_front < EMERGENCY_REVERSE │
       │           ▼                             │
       │       REVERSE ── time>REVERSE_MS ─► REPLAN
       │                                          │
       │                clear & in_state>1.5s     │
       └──────────────────────────────────────────┘
                                                  │ counter>3
                                                  ▼
                                       [escalate NO_PATH]
                                       (REPLAN 持续)

  ARRIVED ◄── distance_to_goal < ARRIVAL_RADIUS_M（任何状态）
  FAULT   ◄── 传感器 stale > 2s 或顶层 NAV_FAILED
```

### 6.2 各状态行为表

| 状态 | steer 输出 | throttle 输出 | 退出条件 |
|------|-----------|--------------|---------|
| CRUISE | `clamp(K_p × heading_error)` (Kp=2.0) | 35% | min_d<60 → AVOID_*；arrived |
| AVOID_LEFT | -100 | 20% | d_front<25 → REVERSE；clear+heading OK → CRUISE；>2s → REPLAN |
| AVOID_RIGHT | +100 | 20% | （对称） |
| REVERSE | 0 | -25% | time>800ms → REPLAN |
| REPLAN | 慢扫 -100→+100→-100，1.5s 内边扫边读 | 0 | 找到 clear 侧 → AVOID_*；扫完仍堵 → escalate NO_PATH |
| PAUSED | 保持上次 | 0 | resume → 恢复 prev_state |
| ARRIVED | 0 | 0 | **进入时触发一次 ARRIVED 事件**（含用时、最终距离、waypoint 名称）；终态，新 nav_goto 才退出 |
| FAULT | 0 | 0 | **进入时触发一次 FAULT 事件**（含失败原因：sensors_lost / abort / unknown）；nav_abort → IDLE |

### 6.3 评分函数（"哪侧更空旷 + 目标加权 + 历史降权"）

进入 AVOID 选择左右时：
```
score(side) = clearance_term + heading_term - history_penalty

clearance_term  = d_side_cm / 200.0                    /* 0..1 */
heading_term    = -|side_bearing_error| / 90.0         /* 偏目标方向加分 */
history_penalty = nav_memory_penalty_for_side(side, cur_lat, cur_lon)
                = (失败次数_最近10米内) / max(1, 总次数)
```

`nav_memory.c` 维护 8 条避障历史环形缓冲，记录每次决策的：进入时距离、目标方位、GPS 位置、是否成功（成功 = 不经 REPLAN 回到 CRUISE）。

### 6.4 escalate 检测代码骨架

```c
typedef enum {
    /* 异常类（带 60s 冷却） */
    ESC_STUCK, ESC_OSCILLATING, ESC_NO_PATH, ESC_LOST, ESC_GOAL_UNREACHABLE,
    /* 终态类（无冷却，仅在状态进入时触发一次） */
    ESC_ARRIVED,    /* 成功到达目标 */
    ESC_ABORTED,    /* 用户/LLM 主动 nav_abort 或 FAULT */
    ESC_COUNT
} escalate_kind_t;

static int64_t s_last_emit_us[ESC_COUNT];

static bool can_emit(escalate_kind_t k) {
    /* 终态类不走冷却，由状态机进入时手动调一次 */
    if (k == ESC_ARRIVED || k == ESC_ABORTED) return true;
    int64_t now = esp_timer_get_time();
    if (now - s_last_emit_us[k] < (int64_t)MIMI_NAV_ESCALATE_COOLDOWN_S * 1000000LL)
        return false;
    s_last_emit_us[k] = now;
    return true;
}

static void detect_stuck(const nav_situation_t *s) {
    static double anchor_lat=0, anchor_lon=0; static int64_t anchor_us=0;
    if (anchor_us==0) { anchor_lat=s->lat; anchor_lon=s->lon;
                        anchor_us=esp_timer_get_time(); return; }
    if (haversine_m(anchor_lat,anchor_lon,s->lat,s->lon) > 0.30) {
        anchor_lat=s->lat; anchor_lon=s->lon;
        anchor_us=esp_timer_get_time(); return;
    }
    if (esp_timer_get_time() - anchor_us > 10*1000000LL && can_emit(ESC_STUCK)) {
        nav_escalate_emit(ESC_STUCK, "STUCK: <30cm in 10s", s);
        anchor_us = esp_timer_get_time();
    }
}
/* detect_oscillating / detect_no_path / detect_lost / detect_goal_unreachable 类似 */
```

---

## 7. 工具层 API（LLM 可调用）

注册流程参照 `tool_pwm_init` 在 `tool_registry.c:222` 的写法。

### 7.1 传感器读取 (`tool_sensors.c`)

| 工具 | Schema | 输出 |
|------|--------|------|
| `read_distance` | `{}` | `{"left_cm":N,"front_cm":N,"right_cm":N,"age_ms":N,"valid":bool}` |
| `read_imu` | `{}` | `{"roll":N,"pitch":N,"yaw":N,"gz_dps":N,"age_ms":N}` |
| `read_gps` | `{}` | `{"lat":..,"lon":..,"fix":bool,"sats":N,"speed_mps":N,"course_deg":N,"age_ms":N}` |

### 7.2 导航控制 (`tool_nav.c`)

| 工具 | Schema | 行为 |
|------|--------|------|
| `nav_goto` | `{lat,lon,speed_pct?}` | 设置绝对 GPS 目标 |
| `nav_goto_waypoint` | `{name,speed_pct?}` | 按名查 waypoint 后启动 |
| `nav_save_waypoint` | `{name,notes?}` | 当前 GPS 存为命名点；无 fix 时拒绝 |
| `nav_list_waypoints` | `{}` | 列表 |
| `nav_delete_waypoint` | `{name}` | 按名删除 |
| `nav_pause` / `nav_resume` | `{}` | 暂停/恢复（保留上下文） |
| `nav_abort` | `{}` | 终止任务回 IDLE |
| `nav_status` | `{}` | `{state,l2,goal,distance_m,bearing_deg,distances,history[3]}` — escalate 后 LLM 主要观察工具 |
| `nav_manual_step` | `{steer_pct,throttle_pct,hold_ms<=1000}` | L3 应急接管一帧，保持后 L2 自动恢复 |

**契约**：所有 nav 命令非阻塞，仅设置 controller 状态；L2 在下个 tick 消费。

---

## 8. 规则引擎扩展

`main/rule_engine/rule_engine.h:8-17` 新增 enum 与字段：
```c
RULE_TRIGGER_ULTRASONIC_DISTANCE,  /* 读 3 路超声波之一 */
RULE_TRIGGER_IMU_TILT,             /* |roll| 或 |pitch| */
RULE_TRIGGER_GPS_DISTANCE_TO,      /* haversine(cur, lat, lon) */

typedef struct {
    rule_trigger_type_t type;
    int pin;
    int channel;        /* 0=left/1=front/2=right 或 0=roll/1=pitch */
    double lat, lon;    /* GPS_DISTANCE_TO 参考点 */
} rule_trigger_t;
```

`rule_engine.c:75-243` 新增 eval 函数 + switch dispatch；`:303-309` `parse_trigger_type` 加新字符串；`:417` `rule_load` 与 `:510` `rule_engine_save` 解析/序列化新字段。

`tool_rule.c:14-138` 与 `tool_registry.c:339-344` 镜像扩展 JSON Schema。

**用例**：LLM 创建规则 "前方 < 30cm 时上报"：
```json
{"name":"close_obstacle","interval_s":1,"cooldown_s":30,
 "trigger":{"type":"ultrasonic_distance","channel":1},
 "condition":{"op":"<","value":30},
 "actions":[{"type":"escalate","escalate_msg":"Front <30cm"}]}
```

---

## 9. escalate 事件路由到 LLM（含到达通知）

### 9.1 关键设计：origin chat_id 透传

为了让"到达后通知用户"能精确回到发起人，`nav_controller` 在收到 `nav_goto` / `nav_goto_waypoint` 工具调用时，需要记录调用上下文的 channel + chat_id 作为 `origin`。

实现方式：扩展 `mimi_tool_t.execute` 调用约定 — 工具注册系统目前不传上下文。最小侵入做法：

- 在 `tool_registry.h` 新增可选 thread-local（或全局加 mutex）`s_current_msg_origin {channel, chat_id}`，由 `agent_loop.c` 在执行工具前设置
- `tool_nav.c` 的 nav_goto 系列从此读取并传给 `nav_controller_set_goal(..., origin)`
- `nav_controller` 把 `origin` 存为运行时状态
- L2 进入 ARRIVED/FAULT 时，`nav_escalate_emit` 把 origin 写进 payload

### 9.2 事件投递

复用 `heartbeat.c:78-105` 模式。`nav_escalate_emit()` 构造 `mimi_msg_t`：
- `channel = origin.channel`（如 MIMI_CHAN_TELEGRAM / MIMI_CHAN_FEISHU / MIMI_CHAN_CLI）— **不再写死 SYSTEM**
- `chat_id = origin.chat_id`（发起人）
- `content = <JSON payload，kind 字段标识事件类型>`
- 调 `message_bus_push_inbound(&msg)` → agent 像处理普通消息一样调 LLM 生成自然语言回复

这样 LLM 生成的回复会自动通过现有的 outbound 分发回到原渠道，无需新建通道。

### 9.3 Payload 示例（异常事件）

```json
{
  "kind": "OSCILLATING",
  "ts_unix": 1731200123,
  "origin": {"channel":"telegram","chat_id":"123456789"},
  "situation": {"lat":37.4221,"lon":-122.0848,
                "distances":{"left":18,"front":12,"right":22},
                "yaw":87.3,"speed_mps":0.0,
                "goal":{"lat":37.4225,"lon":-122.0850,"name":"快递柜","distance_m":47.2}},
  "fsm": {"state":"AVOID_LEFT","prev":"AVOID_RIGHT","time_in_state_ms":1850,
          "avoid_count":3,"replan_count":1},
  "recent_history": [
    {"decision":"AVOID_LEFT","succeeded":false,"duration_ms":2000,"d_front":15},
    {"decision":"AVOID_RIGHT","succeeded":false,"duration_ms":1800,"d_front":18}
  ],
  "hint": "Use read_distance / nav_status. Try nav_manual_step (reverse + opposite steer 800ms), or nav_abort if unsafe."
}
```

### 9.4 Payload 示例（成功到达）

```json
{
  "kind": "ARRIVED",
  "ts_unix": 1731200200,
  "origin": {"channel":"telegram","chat_id":"123456789"},
  "goal": {"lat":37.4225,"lon":-122.0850,"name":"快递柜"},
  "result": {
    "final_distance_m": 2.1,
    "duration_s": 83,
    "total_distance_m": 47.6,
    "avoid_events": 1,
    "escalates_during_trip": 0
  },
  "hint": "Notify the user in natural language. Mention waypoint name, duration, final accuracy."
}
```

### 9.5 Payload 示例（取消/失败）

```json
{
  "kind": "ABORTED",
  "ts_unix": 1731200150,
  "origin": {"channel":"telegram","chat_id":"123456789"},
  "goal": {"name":"快递柜","distance_m_remaining": 12.4},
  "reason": "user_abort",     // 或 "sensors_lost" / "fault"
  "hint": "Notify the user that navigation stopped, include reason."
}
```

### 9.6 NAV_PLAYBOOK.md 决策手册

写到 `/spiffs/config/NAV_PLAYBOOK.md`，由 `context_builder.c` 在 chat_id 中检测到 `nav_*` 类事件时注入 system prompt：

> 收到 NAV 事件时按 kind 处理：
>
> **ARRIVED**：用一句温暖自然的话告诉用户"已到达 {waypoint_name}"，可附用时与最终距离。例："✅ 已到达快递柜啦~ 用时 1 分 23 秒，最终距离目标 2.1 米。"
>
> **ABORTED**：告知用户已停下，附原因。例："已停止前往快递柜（剩余 12.4 米）。原因：用户取消。"
>
> **STUCK / OSCILLATING**：先调 `nav_status` 取最新状态。可尝试 `nav_manual_step`（反向 steer 800ms），失败则 `nav_abort` 并告知用户卡在哪里。
>
> **NO_PATH**：调 `nav_abort` 并询问用户是否换目标点。
>
> **LOST**：重新 `nav_goto` 原目标重规划；若 GPS 长期无 fix 则 `nav_abort` 告知 GPS 问题。
>
> **GOAL_UNREACHABLE**：距目标 <5m 已经算"到了"，调 `nav_abort` 并按 ARRIVED 风格告知用户（强调最终距离）。

---

## 10. SPIFFS 配置文件

### 10.1 `/spiffs/config/sensors.json`
```json
{
  "version": 1,
  "ultrasonic": {
    "left":  {"trig_gpio": 10, "echo_gpio": 12},
    "front": {"trig_gpio": 13, "echo_gpio": 14},
    "right": {"trig_gpio": 15, "echo_gpio": 16},
    "max_range_cm": 400,
    "round_robin_gap_ms": 60
  },
  "imu": {
    "i2c_port": 0, "sda_gpio": 8, "scl_gpio": 9,
    "address": "0x68", "freq_hz": 400000, "sample_hz": 100,
    "gyro_bias_dps": [0.0, 0.0, 0.0]
  },
  "gps": {
    "uart_port": 1, "rx_gpio": 17, "tx_gpio": 18, "baudrate": 9600
  }
}
```

### 10.2 `/spiffs/config/nav.json`
```json
{
  "version": 1,
  "l1": {"emergency_stop_cm": 20, "tick_period_ms": 20},
  "l2": {
    "tick_period_ms": 100,
    "cruise_speed_pct": 35, "avoid_speed_pct": 20, "reverse_speed_pct": 25,
    "avoid_trigger_cm": 60, "emergency_reverse_cm": 25, "clear_cm": 100,
    "avoid_max_ms": 2000, "reverse_ms": 800, "replan_ms": 1500,
    "arrival_radius_m": 3.0,
    "heading_kp": 2.0, "heading_max_steer_pct": 100
  },
  "escalate": {
    "cooldown_s": 60,
    "stuck_window_s": 10, "stuck_distance_m": 0.30,
    "oscillation_window_s": 15, "oscillation_count": 4,
    "lost_distance_m": 20.0, "goal_unreachable_window_s": 30
  }
}
```

### 10.3 `/spiffs/config/waypoints.json`
```json
{"version": 1, "waypoints": []}
```
首次 `nav_save_waypoint` 后追加：
```json
{"name":"快递柜","lat":37.422119,"lon":-122.084801,
 "saved_at":1731200000,"fix_quality":{"sats":9,"hdop":1.2},"notes":""}
```

---

## 11. 分阶段开发计划

按 **风险优先 + 可独立测试** 排序。每个 Phase 在 `serial_cli.c` 加临时 CLI 命令独立验证，最后一并整合。

### Phase 1：HC-SR04 驱动（硬件风险最高，先做）
- **新增**：`drivers/driver_ultrasonic.{h,c}` + `sensor_config.{h,c}`（仅超声波字段）
- **CMake**：加 `esp_driver_rmt`
- **CLI**：`ultrasonic_test` 5Hz 打印 `[L=NN F=NN R=NN]`
- **验收**：30/60/100/200cm 平板测试 ±2cm，连续 30 分钟无 crash，无 <2 或 >400 数据，串扰测试通过

### Phase 2：MPU6050 驱动
- **新增**：`drivers/driver_imu.{h,c}` + sensor_config IMU 字段
- **CMake**：加 `esp_driver_i2c`
- **CLI**：`imu_test` 10Hz 打印 roll/pitch/yaw
- **验收**：±90° 倾角响应 <100ms，静态 60s yaw 漂 <15°，校准后 <5°/分钟

### Phase 3：NEO-6M 驱动
- **新增**：`drivers/driver_gps.{h,c}` + sensor_config GPS 字段
- **CMake**：加 `esp_driver_uart`
- **CLI**：`gps_test` 每条 RMC 打印 lat/lon/sats/fix/speed/course
- **验收**：户外冷启动 <3 分钟首 fix，与手机 GPS 误差 <10m，5 分钟漂移 <10m

### Phase 4：态势结构 + waypoint 存储 + 传感器工具（不动电机）
- **新增**：`nav_situation` / `nav_config` / `nav_waypoints` / `nav_planner`；`tool_sensors.c`；`tool_nav.c`（仅 save/list/delete/status）
- **接入**：`mimi.c` 初始化、`tool_registry.c` 注册
- **验收**：通过 Telegram "现在 GPS"→`read_gps`，"记住这里是测试点"→`nav_save_waypoint`，重启后 "列出 waypoint" 仍在

### Phase 5：L1 反射层（电机首次动起来，复杂度最低）
- **新增**：`nav_l1_reflex` + `nav_controller`（带 dummy goal 测试钩子）
- **验收**：低速直行靠近障碍 → 50ms 内必停（10/10 trial），障碍移走自动释放

### Phase 6：L2 战术层（核心大脑）
- **新增**：`nav_l2_fsm` + `nav_memory`；`tool_nav.c` 补全（goto/pause/resume/abort/manual_step）
- **分级验收**：
  1. 空旷场地 20m 直达 → 误差 <3m
  2. 单障碍 5m 处 → 绕行后回到航向到达
  3. 墙角 → REVERSE+REPLAN 脱出，无碰撞

### Phase 7：escalate 事件管线（含到达通知）
- **新增**：`nav_escalate`；origin 透传机制（`tool_registry` 加 thread-local + `agent_loop` 设置 + `tool_nav` 读取传给 controller）；`mimi.c` 启动 escalate 任务；`context_builder.c` 注入 NAV_PLAYBOOK
- **验收**：
  - **正常到达**：通过 Telegram "去测试点" → 小车自主导航到达 → Telegram **自动收到** "✅ 已到达测试点..." 类回复
  - **主动取消**：行驶中说 "停下" → LLM 调 `nav_abort` → Telegram 收到 "已停止..." 回复
  - **异常事件**：故意制造 STUCK/OSCILLATING/NO_PATH → agent 收到事件并产生合理工具调用 + 通知用户；60s 冷却生效
  - 关键验证：origin 正确，Telegram 发起的导航不会回到 Feishu

### Phase 8：规则引擎扩展 + 收尾
- **改**：rule_engine + tool_rule + tool_registry rule_add schema
- **验收**：LLM 创建 "front<30 escalate" 规则并触发；README 更新；端到端 demo："小车去快递柜" → 单障碍 → 3m 内到达

---

## 12. 验证策略

### 12.1 驱动层台架测试（不装车）
| 驱动 | 台架 | 通过标准 |
|------|------|---------|
| HC-SR04 | 30/60/100/200/300cm 平板 | ±2cm，3 路一致 |
| MPU6050 | 平面静置 + ±45° 倾斜 | 静漂 <5°/60s，动响 <100ms |
| NEO-6M | 户外冷启动 | <3 分钟 fix，sats ≥6 |
| 态势结构 | CLI 100×10ms 转储 | 并发驱动下无撕裂 |

### 12.2 L2 桌面测试（mock 传感器）
加 CLI `nav_l2_test`：替换 `nav_situation_get` 为脚本读数，10Hz 驱动 FSM 回放场景，比对状态转移：
- **straight_clear**：全 400cm，目标 20m N → 应 CRUISE 全程到达
- **front_blocked**：d_front=15, d_left=200, d_right=80 → 应 AVOID_LEFT
- **boxed_in**：全 15cm → REVERSE → REPLAN → 3 次后 escalate NO_PATH
- **flapping**：左右轮换堵 → ≤4 次切换内 escalate OSCILLATING

### 12.3 整车场地测试
1. 直线 20m → 终点误差
2. 5m 处单箱 → 绕行宽度 + 到达时间
3. 双箱通道 → 穿过无碰
4. 墙角 → REVERSE+REPLAN 脱出
5. 障碍后目标 → 30s 内 GOAL_UNREACHABLE escalate
6. 树荫 GPS 弱信号 → FAULT 安全停车

---

## 13. 关键风险与缓解

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| HC-SR04 串扰 | 高 | 假障碍/震荡 | 严格 60ms 轮询 + ±30° 物理分散 + 最近 3 帧中值滤波 |
| MPU6050 yaw 漂 | 必然 | 航向错 | GPS course 在 speed>0.5 时校正；开机 bias 校准 |
| NEO-6M 冷启慢 | 高 | 首 fix 慢 | 写 `gps_lkg.json` 暖启动；启动期日志 "waiting for fix" |
| 电机 EMI 干扰超声波 | 中 | 速度高时数据乱 | 回波线双绞 + 传感器 VCC LC 滤波；电机开关对照测试 |
| L1 任务被饿死 | 低 | 紧急停车迟到 | L1 prio=7 高于 agent_loop=6；定期采样 stack high water mark |
| escalate 风暴 | 中 | 浪费 token | 每类 60s 冷却；PAUSED 时抑制；每分钟全量上限 3 次 |
| 互斥锁争抢 | 低 | L1 错过 deadline | hold time <5µs，必要时降级为 spinlock |
| L2 栈溢出 | 中 | crash | 6KB 起步，监控 high water，余量 <1KB 即扩 |
| OTA 行驶中触发 | 低 | 行驶中重启 | OTA 工具描述提示先 nav_abort（强制由 Phase 8 决定） |

---

## 关键复用清单（对应现有代码）

- **工具注册模板**：`main/tools/tool_pwm.c:222-326`（init + JSON Schema + execute）
- **SPIFFS+cJSON 配置加载**：`main/tools/tool_pwm.c:178-232`（`rc_load_config`）
- **FreeRTOS 后台任务**：`main/heartbeat/heartbeat.c:115-148`（while + vTaskDelay + xTaskCreate）
- **消息总线推送给 agent**：`main/heartbeat/heartbeat.c:78-105`（`message_bus_push_inbound`）
- **底盘控制**：`main/tools/tool_pwm.c:rc_steer/rc_throttle:327-419`（不重做）
- **GPIO 白名单校验**：`main/tools/gpio_policy.c:gpio_policy_pin_is_allowed`（外设引脚分配前先校验）

## 关键修改入口文件

- `/Users/yinbo/AI_Project/mimiclaw/main/CMakeLists.txt`
- `/Users/yinbo/AI_Project/mimiclaw/main/mimi_config.h`
- `/Users/yinbo/AI_Project/mimiclaw/main/mimi.c`
- `/Users/yinbo/AI_Project/mimiclaw/main/tools/tool_registry.c`
- `/Users/yinbo/AI_Project/mimiclaw/main/rule_engine/rule_engine.c` 与 `.h`
- `/Users/yinbo/AI_Project/mimiclaw/main/tools/tool_rule.c`

加上新建的 ~22 个文件（`main/drivers/*`、`main/nav/*`、`main/tools/tool_sensors.c`、`main/tools/tool_nav.c`）和 3 个 SPIFFS 配置（`spiffs_data/config/{sensors,nav,waypoints}.json`）。
