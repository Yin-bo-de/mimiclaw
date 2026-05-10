# MimiClaw 各 Phase 测试方法（执行层面）

## 准备工作：连接与烧录

**两个 USB-C 口的用途：**
| 口 | 端口 | 用途 |
|----|------|------|
| USB（JTAG） | `/dev/cu.usbmodem21201` | `idf.py flash`、日志、REPL CLI |
| COM（UART） | `/dev/cu.usbmodem11206` | 备用串口输出 |

**每个 Phase 测试前的标准流程：**
```bash
# 1. 编译
/Users/yinbo/.espressif/python_env/idf5.5_py3.9_env/bin/python \
  /Users/yinbo/.espressif/esp-idf-v5.5.2/tools/idf.py build

# 2. 烧录 + 进入 monitor（REPL 提示符在这里出现）
/Users/yinbo/.espressif/python_env/idf5.5_py3.9_env/bin/python \
  /Users/yinbo/.espressif/esp-idf-v5.5.2/tools/idf.py \
  -p /dev/cu.usbmodem21201 flash monitor
```

烧录完毕后，你会看到 `mimi>` 提示符，在这里输入 CLI 命令。

**退出 monitor**：`Ctrl+]`

---

## Phase 1：HC-SR04 超声波驱动

### 目标
验证三路超声波传感器正确测距，无串扰，无崩溃。

### 步骤 1：基础连通测试

烧录后在 `mimi>` 提示符输入：
```
ultrasonic_test -c 5 -d 500
```

**预期输出：**
```
HC-SR04 Ultrasonic Test - 5 samples, 500ms delay
001: Left= 45cm, Front= 30cm, Right= 60cm  [V:1,1,1]  Age=12ms
002: Left= 44cm, Front= 31cm, Right= 59cm  [V:1,1,1]  Age=13ms
...
```

**判断通过：**
- `[V:1,1,1]` —— 三路全部 `valid=1`
- `Age` 值 < 500ms
- 数值在 2~400 之间（不出现 0、负数、999）

**判断失败：**
- `[V:0,0,0]` → GPIO 接线错误，检查 TRIG/ECHO 是否接反，或 VCC 是否供电
- 数值全为 400 → 传感器触发了但回波没收到，检查 ECHO 引脚
- `assert` 崩溃 → RMT 通道冲突，查 `sensors.json` 中 GPIO 配置

### 步骤 2：精度验证

用硬纸板/书本放在前方，手动量好距离，对比传感器读数：

```
# 测 30 次，每次 200ms，观察稳定性
ultrasonic_test -c 30 -d 200
```

| 物理距离 | 可接受读数范围 |
|---------|------------|
| 30cm | 28~32cm |
| 60cm | 58~62cm |
| 100cm | 98~102cm |
| 200cm | 196~204cm |

### 步骤 3：串扰测试

三个传感器对准同一块板，确认读数一致，不出现 "一个 sensor 读到另一个 sensor 的回波"（表现为某路数据忽然变成另一路的值）：
```
ultrasonic_test -c 20 -d 200
```
三路读数之差不应超过物理偏角导致的合理差值（各方向角 ±30° 最多差几 cm）。

### 步骤 4：长时间稳定性

```
# 连续 360 次，1 秒一次 = 6 分钟
ultrasonic_test -c 360 -d 1000
```

**通过标准：** 全程无 `[V:0,x,x]` 翻转，无 `Guru Meditation Error`，无 watchdog 超时。

---

## Phase 2：MPU6050 IMU 驱动

### 目标
验证加速度计、陀螺仪正常工作，roll/pitch 响应正确，yaw 漂移在可接受范围。

### 步骤 1：基础连通测试

```
imu_test -c 5 -d 500
```

**预期输出（板子平放时）：**
```
MPU6050 IMU Test - 5 samples, 500ms delay
001: Roll=  0.2°, Pitch=  0.1°, Yaw=  0.0°  [Accel x=-0.01g y= 0.02g z= 1.00g]  [Gyro x= 0.05°/s y=-0.03°/s z= 0.02°/s]  [V:1]  Age=8ms
```

**判断失败：**
- `[V:0]` → I2C 通信失败，检查 SDA(GPIO8)/SCL(GPIO9) 接线，用万用表量 I2C 上拉电阻（3.3V 侧需要 4.7kΩ 上拉）
- `Accel z ≈ 0g`（平放时 Z 轴应接近 ±1g）→ 传感器方向不对或数据字节序错
- `Age` 持续 >100ms → 驱动任务被饿死，查 FreeRTOS 任务优先级

### 步骤 2：倾斜响应测试

板子平放，先记录基准值；然后手持板子：

```
imu_test -c 20 -d 100
```

- 绕 X 轴旋转 90°（让板子竖起来）→ `Roll` 应变化 ~90°
- 绕 Y 轴旋转 45° → `Pitch` 应变化 ~45°
- 响应时间：从你物理旋转到数值变化，视觉上应 <100ms（0.1 秒内）

### 步骤 3：yaw 漂移测试

板子完全静止放平，连续采集 60 秒：

```
imu_test -c 60 -d 1000
```

对比第 1 条和第 60 条的 `Yaw` 值：
- **通过**：|Yaw₆₀ - Yaw₁| < 15°（方案要求 60s 内漂 <15°）
- **最优**：完成陀螺仪 bias 校准后 <5°/分钟

### 步骤 4：开机校准验证

重启板子，观察 monitor 日志（烧录后 5 秒内）：

```
I (XXXX) driver_imu: Calibrating gyro bias (5s static required)...
I (XXXX) driver_imu: Gyro bias: x=0.012 y=-0.008 z=0.003 dps
I (XXXX) driver_imu: IMU driver started
```

若日志显示校准成功，重复步骤 3，yaw 漂应更小。

---

## Phase 3：NEO-6M GPS 驱动

**注意：GPS 需要在户外开阔天空下测试，室内无法 fix。**

### 步骤 1：基础 UART 通信测试（可室内）

烧录后在 monitor 日志中观察：

```
I (XXXX) driver_gps: GPS driver started on UART1 (RX=17, TX=18) at 9600 baud
```

然后在 CLI 输入（室内，此时无 fix）：
```
gps_test -c 5 -d 1000
```

**预期室内输出（无 fix 时）：**
```
NEO-6M GPS Test - 5 samples, 1000ms delay
001: Lat=  0.000000°, Lon=  0.000000°, Alt=   0.0m, Speed=  0.0m/s, Course=  0.0°  [Sats= 0]  [V:0]  Age=850ms
```

- `[V:0]` 是正常的（无 fix）
- `Age` < 2000ms → 说明 GPS UART 数据在流动（即使无 fix，GPS 也在发 NMEA 句子）
- 若 `Age` 持续 >5000ms → GPS 没有数据输出，检查 RX(GPIO17) 接线是否是接 GPS 的 **TX** 引脚

### 步骤 2：户外 fix 测试

**将小车（或开发板）带到室外开阔处**（无高楼遮挡），连接笔记本运行 monitor，等待：

```
gps_test -c 60 -d 5000   # 每5秒一次，最多等5分钟
```

**成功 fix 的输出：**
```
015: Lat= 31.123456°, Lon=121.456789°, Alt=  12.3m, Speed=  0.0m/s, Course=  0.0°  [Sats= 7]  [V:1]  Age=200ms
```

- `[V:1]` 出现 → fix 成功
- 冷启动应在 3 分钟内出现 `[V:1]`
- `Sats` ≥ 6 为良好信号

**对比手机 GPS：**
打开手机「指南针」或「地图」查看当前坐标，与传感器读数对比，差值应 <10m。

### 步骤 3：GPS 数据连续性测试

fix 后运行：
```
gps_test -c 30 -d 2000
```

每条记录的 `Age` 应 < 1200ms（NEO-6M 默认 1Hz 更新，每秒一条 NMEA），若 `Age` 忽然跳到 >3000ms 说明有数据丢帧，检查 UART 波特率是否正确（9600）。

---

## Phase 4：态势结构 + Waypoint + Sensor Tools

**此 Phase 不涉及电机，可在桌面测试。**

### 步骤 1：验证 sensor tools 通过 Telegram 可用

在 **Telegram** 向 MimiClaw 发送：
```
现在传感器读数是多少？
```

LLM 应调用 `read_distance` 并回复类似：
```
当前超声波读数：左 45cm，前 80cm，右 62cm，数据新鲜度 23ms，有效。
```

再发：
```
现在 GPS 在哪里？
```

LLM 调用 `read_gps` 回复当前经纬度（需户外有 fix）或提示当前无 GPS 信号。

**失败排查：**
- LLM 回复「工具不存在」→ `tool_registry.c` 注册失败，查启动日志 `Registering tool: read_distance`
- 数据全为 0 / valid=false → Phase 1~3 驱动未正常启动

### 步骤 2：验证 `nav_save_waypoint`（需户外 fix）

在户外有 GPS fix 的情况下，通过 Telegram 发：
```
记住这里是"测试点A"
```

LLM 应调用 `nav_save_waypoint({"name":"测试点A"})` 并回复：
```
已保存航点"测试点A"（纬度 31.123456, 经度 121.456789，卫星数 8）。
```

### 步骤 3：验证 Waypoint 持久化（重启后仍在）

**在 CLI 中重启：**
```
restart
```

重启后通过 Telegram 发：
```
有哪些已保存的地点？
```

LLM 调用 `nav_list_waypoints`，应仍然列出「测试点A」。

**通过标准：** 重启后 waypoint 依然存在，说明 SPIFFS 写入正常。

### 步骤 4：通过 `tool_exec` 直接测试 nav_status

在 CLI 中：
```
tool_exec nav_status {}
```

**预期输出：**
```json
{"state":"IDLE","l2":"IDLE","goal":null,"distance_m":0.0,"bearing_deg":0.0}
```

---

## Phase 5：L1 反射层（电机首次启动）

**⚠️ 安全准备：小车此时会真实运动。将小车放在开阔平地，手边准备随时断电。**

### 步骤 1：确认 L1 已启动

查看启动日志（在 `idf.py flash monitor` 输出中）：
```
I (XXXX) nav_l1: L1 reflex task started (50 Hz, emergency_stop_cm=20)
```

### 步骤 2：启动 dummy drive，验证正常行进

```
l1_test -s 20
```

小车应以 **20% 油门**直线前进（速度很慢，安全）。

**观察点：**
- 小车实际前进（电机转动）
- monitor 日志无错误

### 步骤 3：触发紧急停车

在小车前进时，将手/障碍物放到任意一个超声波传感器 **20cm 以内**：

**预期行为：** 小车在 **50ms 内**停止（L1 以 50Hz 检测，最多 20ms 一个周期，加上机械惯量合计 <50ms）

**预期 monitor 日志：**
```
W (XXXX) nav_l1: EMERGENCY STOP triggered: min_d=12cm < 20cm
```

### 步骤 4：验证障碍移走后自动恢复

将障碍物撤走（距离 >20cm）：

**预期行为：** L1 停止 block，dummy drive 任务让小车重新前进

**预期日志：**
```
I (XXXX) nav_l1: Obstacle cleared, releasing block
```

### 步骤 5：重复验证 10 次

连续进行 10 次「放障碍物 → 停车 → 撤走 → 恢复」：

```
# 每次不需要重新输入命令，l1_test 一直在后台运行
# 只需移动障碍物
```

**通过标准：** 10/10 次都在障碍物进入 20cm 后停车。

### 步骤 6：停止 dummy drive

```
l1_test -x
```

---

## Phase 6：L2 战术层（自主导航）

**分三个场景，由简到难。**

### 场景 A：20m 空旷直达

**场地要求：** 20m 以上直线，两侧无障碍物。

#### 步骤 1：保存起点和终点 waypoint（户外 fix 后）

通过 Telegram：
```
记住这里是"起点"
```
驾车到 20m 外：
```
记住这里是"终点20m"
```
开车回起点，将小车放在起点。

#### 步骤 2：发送导航指令

```
去"终点20m"
```

LLM 调用 `nav_goto_waypoint({"name":"终点20m"})`，monitor 日志应出现：
```
I (XXXX) nav_ctrl: Goal set: 终点20m @ (31.xxx, 121.xxx)
I (XXXX) nav_l2: State: IDLE -> CRUISE
```

#### 步骤 3：观察行为

小车以 35% 油门前进，航向 PID 修正方向。

**通过标准：**
- 小车到达终点（距目标点 <3m）
- Telegram 自动收到消息："✅ 已到达终点20m..."
- 全程未触发 L1 紧急停车（无障碍）

### 场景 B：单障碍绕行

**场地：** 起点到目标点之间放一个纸箱（约 50cm × 50cm）。

#### 步骤 1：发送导航指令

```
去"终点20m"
```

**监控 L2 状态转移（monitor 日志）：**
```
I nav_l2: State: CRUISE -> AVOID_LEFT  (front=18cm, score: L=0.72 R=0.45)
I nav_l2: State: AVOID_LEFT -> CRUISE  (clear, heading_ok)
```

**通过标准：**
- 小车绕开障碍物（不碰到）
- 绕过后回归正确航向
- 最终到达目标点（<3m）

### 场景 C：墙角 REVERSE+REPLAN

**场地：** 在小车前方和一侧同时放障碍物，模拟"被夹住"。

#### 步骤 1：手动模拟

导航运行中，手动将障碍物从侧面夹住小车（或直接启动导航后在出发前就用两块板子夹住）。

**预期日志序列：**
```
I nav_l2: State: CRUISE -> AVOID_LEFT   (front blocked)
W nav_l2: State: AVOID_LEFT -> REVERSE  (front < 25cm emergency)
I nav_l2: State: REVERSE -> REPLAN      (800ms elapsed)
I nav_l2: REPLAN scanning...
```

**通过标准：**
- 小车尝试后退后重新扫描
- 如果两侧都被堵死（ >3次 REPLAN），触发 `escalate NO_PATH`，Telegram 收到通知

---

## Phase 7：Escalate 事件管线

### 测试 A：正常到达通知

执行场景 A（20m 空旷）的完整导航，到达后：

**验证：** Telegram（**发起导航的那个会话**）自动收到如下消息（不需要你发任何消息）：
```
✅ 已到达终点20m~ 用时 1 分 05 秒，最终距离目标 1.8 米。
```

**关键验证：** 如果你从 Telegram 发起，通知必须回到 Telegram；绝对不会出现在飞书或 CLI。

### 测试 B：主动取消通知

导航进行中，通过 Telegram 发：
```
停下
```

LLM 调用 `nav_abort({})`，Telegram 收到：
```
已停止前往终点20m（剩余约 8.2 米）。原因：用户取消。
```

### 测试 C：STUCK 事件（人为制造）

导航启动后，**物理固定小车**（手按住或卡轮子）不让它动，等待 10 秒：

**预期：** Telegram 收到 STUCK 通知，LLM 会先调用 `nav_status` 查情况，然后尝试 `nav_manual_step` 或 `nav_abort`。

**日志：**
```
W nav_escalate: ESC_STUCK emitted (no movement > 10s)
```

### 测试 D：60s 冷却验证

STUCK 触发后，继续固定小车不让其移动，观察是否在 60 秒内**不会**再次触发 STUCK 通知（冷却生效）。60 秒后，应再次触发。

### 测试 E：OSCILLATING 事件

在小车前进路上放一块板，左右两侧各放一块挡板（迫使小车在 AVOID_LEFT 和 AVOID_RIGHT 之间反复切换）：

在 15 秒内切换 ≥4 次 → Telegram 收到 OSCILLATING 通知。

---

## Phase 8：规则引擎扩展

### 测试 A：创建超声波触发规则

通过 Telegram 发：
```
创建一条规则：当前方超声波 < 30cm 时，给我发通知，冷却 30 秒
```

LLM 应调用 `rule_add`，参数类似：
```json
{
  "name": "close_obstacle",
  "interval_s": 1,
  "cooldown_s": 30,
  "trigger": {"type": "ultrasonic_distance", "channel": 1},
  "condition": {"op": "<", "value": 30},
  "actions": [{"type": "escalate", "escalate_msg": "前方障碍物 <30cm"}]
}
```

**验证：** 将手放到前方超声波 25cm 处，Telegram 应收到通知。再次靠近（30 秒内），不应再次通知（冷却中）。30 秒后再靠近，再次通知。

### 测试 B：端到端 Demo

**场地：** 约 30m 路径，中间放 1 个纸箱障碍物。

1. 通过 Telegram：`记住这里是"快递柜"`（先把小车开到目标点保存，再回来）
2. 发送：`小车去快递柜`
3. 观察：小车自主出发 → 遇障绕行 → 到达 → Telegram 自动回复到达通知

**最终验收标准：** 全流程无人工干预，在 3m 误差内到达，Telegram 收到自然语言到达通知。

---

## 快速参考：CLI 命令汇总

| 命令 | 用途 |
|------|------|
| `ultrasonic_test -c 10 -d 500` | 超声波读数 10 次 |
| `imu_test -c 30 -d 1000` | IMU 读数 30 次 |
| `gps_test -c 20 -d 2000` | GPS 读数 20 次 |
| `l1_test -s 20` | 启动 20% 油门 dummy drive |
| `l1_test -x` | 停止 dummy drive |
| `tool_exec nav_status {}` | 查看当前导航状态 |
| `tool_exec nav_abort {}` | 立即停止导航 |
| `tool_exec read_distance {}` | 直接读超声波 |
| `tool_exec nav_list_waypoints {}` | 列出所有 waypoint |
| `heap` | 查看剩余堆内存 |
| `restart` | 重启板子 |

---

## GPIO 接线速查表

| 用途 | GPIO | 接传感器引脚 |
|------|------|------------|
| MPU6050 SDA | 8 | SDA |
| MPU6050 SCL | 9 | SCL |
| GPS RX | 17 | GPS TX |
| GPS TX | 18 | GPS RX |
| 超声波左 TRIG | 10 | TRIG |
| 超声波左 ECHO | 12 | ECHO |
| 超声波前 TRIG | 13 | TRIG |
| 超声波前 ECHO | 14 | ECHO |
| 超声波右 TRIG | 15 | TRIG |
| 超声波右 ECHO | 16 | ECHO |
| 舵机 | 11 | 已占用 |
| 电调 | 21 | 已占用 |

---

**建议测试顺序：** Phase 1 → 2 → 3 → 4（桌面）→ 5（空地，低速）→ 6A → 6B → 6C → 7 → 8。每个 Phase 验收通过再进下一个，Phase 1~3 可以并行（不依赖彼此），但 5~8 强依赖前面全部完成。
