# MimiClaw 导航选路逻辑文档

本文档描述 `nav_l2_fsm.c` 中 L2 层有限状态机（FSM）的选路决策逻辑，涵盖状态划分、传感器使用、评分机制和历史记忆。

---

## 1. 总体架构

L2 层状态机以 **50 ms 周期**（`MIMI_NAV_L2_PERIOD_MS`）运行，核心职责：

- 在无障碍物时保持 **CRUISE**（巡航），朝目标点行驶。
- 前方出现障碍物时，决策 **左转** 或 **右转** 进入 AVOID 状态绕行。
- 绕行失败时进入 **REPLAN**（重新规划）扫舵寻找新方向。
- 极端情况触发 **REVERSE**（后退）和 **FAULT**（故障停机）。

状态转换图（简化）：

```
                    ┌─────────────┐
                    │   CRUISE    │
                    └──────┬──────┘
                           │ 前方有障
                           ▼
              ┌────────────────────────┐
              │  score_sides 决策左转/右转  │
              └────────────┬───────────┘
                           │
              ┌────────────┴────────────┐
              ▼                         ▼
       ┌──────────┐              ┌──────────┐
       │AVOID_LEFT│              │AVOID_RIGHT│
       └────┬─────┘              └────┬─────┘
            │ 前方clear/超时/         │ 前方clear/超时/
            │ 过近触发REVERSE         │ 过近触发REVERSE
            ▼                         ▼
       ┌─────────┐              ┌─────────┐
       │ CRUISE  │              │ CRUISE  │
       │(成功)   │              │(成功)   │
       └─────────┘              └─────────┘
            │                         │
            │ 超时/失败                │ 超时/失败
            ▼                         ▼
       ┌─────────┐              ┌─────────┐
       │ REPLAN  │◄────────────►│ REPLAN  │
       └────┬────┘              └────┬────┘
            │                         │
            └──────────┬──────────────┘
                       │ 多次失败
                       ▼
                  ┌─────────┐
                  │  FAULT  │
                  └─────────┘
```

---

## 2. 状态定义

| 状态 | 名称 | 说明 |
|------|------|------|
| `L2_CRUISE` | 巡航 | 正常行驶，朝目标点打舵 |
| `L2_AVOID_LEFT` | 左转绕行 | 遇障后选择左转 |
| `L2_AVOID_RIGHT` | 右转绕行 | 遇障后选择右转 |
| `L2_REVERSE` | 后退 | 前方过近时紧急后退 |
| `L2_REPLAN` | 重新规划 | 扫舵寻找 clear 方向 |
| `L2_PAUSED` | 暂停 | 用户暂停，停止油门 |
| `L2_ARRIVED` | 到达 | 到达目标点半径内 |
| `L2_FAULT` | 故障 | 传感器丢失或多次重试失败，停机 |

---

## 3. 正常巡航遇障的选路逻辑（`score_sides`）

当 `fsm_cruise` 检测到 `min_d < avoid_trigger_cm` 时，调用 `score_sides` 对左转/右转进行三维度评分。

### 3.1 评分公式

```c
float score_left  = cl + hl - pl;
float score_right = cr + hr - pr;
return (score_left >= score_right) ? 0 : 1;   // 0=左转, 1=右转
```

### 3.2 三个维度详解

#### ① 空间开阔度（clearance）

```c
float cl = (float)sit->distances_cm[0] / 200.0f;   // 左边距离 / 200
float cr = (float)sit->distances_cm[2] / 200.0f;   // 右边距离 / 200
```

- 直接使用左/右超声波测距值，归一化到 **200 cm**。
- 距离越大，该维度得分越高。
- 若某侧距离为 0（无效），该维度得分为 0。

#### ② 航向收益（heading）

```c
double left_bear  = fmod(bearing_to_goal - 30.0 + 360.0, 360.0);
double right_bear = fmod(bearing_to_goal + 30.0,          360.0);
double left_err   = nav_planner_heading_error_deg(sit->yaw_deg, left_bear);
double right_err  = nav_planner_heading_error_deg(sit->yaw_deg, right_bear);
float hl = -(float)fabs(left_err)  / 90.0f;
float hr = -(float)fabs(right_err) / 90.0f;
```

- 假设左转/右转后航向偏移 ±30°，计算新航向与目标航向的误差。
- 误差越小得分越高（误差 0° 得 0 分，误差 90° 得 -1 分）。
- **这里用到了陀螺仪 yaw（航向角）**。
- 若 GPS 无效（无目标点），以当前 `yaw_deg` 作为 `bearing_to_goal` 传入，此时航向收益退化为"谁更接近当前航向"。

#### ③ 历史惩罚（memory penalty）

```c
float pl = nav_memory_penalty_for_side(0, sit->lat, sit->lon);
float pr = nav_memory_penalty_for_side(1, sit->lat, sit->lon);
```

- 查询 `nav_memory`，在当前位置 **10 米范围**内，该侧历史避障记录中 **失败率**（`失败次数 / 总次数`）。
- 失败率越高，惩罚越大（0.0 ~ 1.0）。
- 环形缓冲区最多保存 **8 条**记录。

### 3.3 设计意图

不单纯选"空的一边"，而是综合：
1. **物理空间** — 哪边更宽
2. **目标对齐** — 转完后是否更对准目标
3. **历史教训** — 这里以前走这边成功过吗

---

## 4. 重新规划路径的选路逻辑（`fsm_replan`）

当 AVOID 超时或扫舵完成后仍未找到 clear 方向，进入 `REPLAN`。此时不使用评分，而是按 **固定优先级** 判断：

```
1. 前方距离 >= clear_cm     → 回到 CRUISE
2. 左边距离 >= clear_cm     → AVOID_LEFT（优先级高于右边）
3. 右边距离 >= clear_cm     → AVOID_RIGHT
4. 都不满足                 → 继续扫舵 / 再次 REPLAN
```

**注意**：REPLAN 状态下 `replan_count` 会递增，超过 **3 次**直接进入 `FAULT`。

---

## 5. 传感器数据的使用

### 5.1 陀螺仪（IMU）

| 字段 | 是否用于选路 | 作用 |
|------|-------------|------|
| `yaw_deg`（航向角） | ✅ 是 | ① 巡航时计算舵量（`heading_error_deg`）；② `score_sides` 中评估转向后的航向收益 |
| `roll_deg`（横滚角） | ❌ 否 | 仅采集保存，未参与决策 |
| `pitch_deg`（俯仰角） | ❌ 否 | 仅采集保存，未参与决策 |
| `gz_dps`（Z轴角速度） | ❌ 否 | 仅采集保存，未参与决策 |

### 5.2 GPS

| 字段 | 是否用于选路 | 作用 |
|------|-------------|------|
| `lat/lon` | ✅ 是 | 计算到目标点的距离和方位角（`bearing`） |
| `fix_valid` / `gps_fix` | ✅ 是 | 无 fix 时无法计算目标方位，巡航舵量为 0，AVOID fallback 用当前 yaw |
| `speed_mps` | ❌ 否 | 仅采集保存 |
| `course_deg` | ❌ 否 | 仅采集保存 |
| `sats` | ❌ 否 | 仅采集保存（用于搜星观察） |

### 5.3 超声波

| 字段 | 是否用于选路 | 作用 |
|------|-------------|------|
| `distances_cm[0]`（左） | ✅ 是 | `score_sides` 的空间开阔度 + `fsm_replan` 判断 |
| `distances_cm[1]`（前） | ✅ 是 | 触发 AVOID / REVERSE / 退出 AVOID |
| `distances_cm[2]`（右） | ✅ 是 | `score_sides` 的空间开阔度 + `fsm_replan` 判断 |

---

## 6. 历史记忆机制（`nav_memory`）

### 6.1 数据结构

```c
typedef struct {
    double lat, lon;       // 发生位置
    int    side;           // 0=left, 1=right
    bool   succeeded;      // 最终是否成功回到 CRUISE
    int    d_front_cm;     // 进入时的前方距离
    int64_t ts_us;         // 时间戳
    uint32_t duration_ms;  // 在 AVOID 中停留的时长
    bool   valid;
} avoid_record_t;
```

### 6.2 生命周期

1. **进入 AVOID 时**（`fsm_cruise` 中 `nav_memory_record_attempt`）：
   - 记录当前位置、选择的边、前方距离。
   - `succeeded = false`（尚未完成）。

2. **离开 AVOID 时**（`on_leave_avoid` 中 `nav_memory_mark_last_result`）：
   - 如果成功回到 `CRUISE`，`succeeded = true`。
   - 否则（进入 `REPLAN`/`REVERSE`/`FAULT`），`succeeded = false`。
   - 记录耗时 `duration_ms`。

### 6.3 惩罚计算

```c
penalty = failed_count / total_count   // 仅统计 10 米内的同侧记录
```

- 新记录会覆盖最旧的（环形缓冲区，容量 8）。
- 惩罚是局部经验学习，避免在相似位置重复选择失败方向。

---

## 7. 各状态行为速查

| 状态 | 油门 | 舵量 | 退出条件 |
|------|------|------|----------|
| **CRUISE** | `s_cruise_speed`（默认 35%） | `heading_kp * heading_error` | 到达目标 → ARRIVED；前方有障 → AVOID |
| **AVOID_LEFT** | `avoid_speed_pct` | `-100`（满舵左） | 前方 clear 且 settle > 300 ms → CRUISE；前方过近 → REVERSE；超时 → REPLAN |
| **AVOID_RIGHT** | `avoid_speed_pct` | `+100`（满舵右） | 同上 |
| **REVERSE** | `-reverse_speed_pct` | `0` | 持续 `reverse_ms` 后 → REPLAN |
| **REPLAN** | `0` | 扫舵（-100 → +100 → -100） | 前/左/右任一侧 clear → 对应状态；超时且 replan_count > 3 → FAULT |
| **PAUSED** | `0` | 保持 | 用户 resume → 恢复上一状态 |
| **ARRIVED** | `0` | `0` | 需用户设置新目标 |
| **FAULT** | `0` | `0` | 需用户重启导航 |

---

## 8. 关键参数（`nav_config_t`）

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `arrival_radius_m` | — | 到达判定半径（米） |
| `avoid_trigger_cm` | — | 触发避障的最小前方距离 |
| `clear_cm` | — | 判定"前方 clear"的阈值 |
| `emergency_reverse_cm` | — | 触发紧急后退的阈值 |
| `avoid_speed_pct` | — | AVOID 状态巡航速度 |
| `avoid_max_ms` | — | AVOID 超时时间 |
| `reverse_speed_pct` | — | 后退速度百分比 |
| `reverse_ms` | — | 后退持续时间 |
| `replan_ms` | — | REPLAN 扫舵总时长 |
| `heading_kp` | — | 航向误差到舵量的比例系数 |
| `heading_max_steer_pct` | — | 最大舵量限制 |

---

## 9. 日志观察

新增的状态动作日志（状态切换时打印一次）：

```
决策：前进 (throttle=35%)
决策：左转 (steer=-100)
决策：右转 (steer=100)
决策：后退 (throttle=-20%)
决策：重新规划路径
```

巡航时的航向日志（每周期打印）：

```
NAV: goal_bearing=123.4° yaw=120.1° error=3.3° steer=8
```

---

*文档版本：2025-05-11*  
*对应代码：`main/nav/nav_l2_fsm.c`、`main/nav/nav_memory.c`、`main/nav/nav_planner.c`*
