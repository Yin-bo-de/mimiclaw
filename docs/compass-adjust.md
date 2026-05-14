# 方向对齐原理与验证方法

## 问题根因

MPU6050 是六轴 IMU（陀螺仪 + 加速度计），**没有磁力计**，无法感知绝对方向。

开机时 `s_yaw = 0.0f`（`driver_imu.c:246`），这个 0° 表示的是"开机瞬间车头所朝的方向"，而不是"真北"。导航层的 `nav_planner_bearing_deg()` 计算出的目标方位角是基于真北的 compass 坐标系。两者坐标系不同，恒定偏差 = 开机时车头与真北的夹角。

```
实际误差 = φ_boot（开机方向与真北的夹角）
```

若开机方向恰好朝南（φ_boot ≈ 180°），导航会始终驱车朝反方向行驶。

---

## 一、目标方位角是怎么计算出来的

### 1.1 坐标约定

整个导航层采用统一约定：**0° = 真北，顺时针为正，范围 [0°, 360°)**。目标方位角（bearing）和 IMU yaw 都必须在这个坐标系下才能相减。

### 1.2 Haversine 大圆方位角公式

`nav_planner.c:19` 的 `nav_planner_bearing_deg(lat1, lon1, lat2, lon2)` 用标准球面三角公式计算从当前位置到目标的真实地理方位：

```c
double dlon = (lon2 - lon1) * DEG_TO_RAD;
double y = sin(dlon) * cos(lat2 * DEG_TO_RAD);
double x = cos(lat1 * DEG_TO_RAD) * sin(lat2 * DEG_TO_RAD)
         - sin(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) * cos(dlon);
double bearing = atan2(y, x) * RAD_TO_DEG;
return fmod(bearing + 360.0, 360.0);
```

**逐行解释：**

- `dlon`：目标与当前位置的经度差（转弧度）。
- `y = sin(dlon) * cos(lat2)`：方位角的东西分量。当目标在正东方向（dlon=90°），y 最大；正西最小。
- `x = cos(lat1)*sin(lat2) - sin(lat1)*cos(lat2)*cos(dlon)`：方位角的南北分量。当目标在正北（纬度差为正且 dlon=0），x 最大；正南 x 最小。
- `atan2(y, x)`：用四象限反正切还原出 [-180°, +180°] 范围的角度，其中 0° 对应正北。
- `fmod(bearing + 360, 360)`：将负值区间折叠到 [0°, 360°)，确保输出永远是正数。

**举例**：当前在 (30°N, 100°E)，目标在 (30°N, 101°E)（正东 1°），代入后 y > 0，x ≈ 0，atan2 ≈ 90°，输出 bearing = 90°，即正东——符合直觉。

### 1.3 航向误差计算

`nav_planner.c:29` 的 `nav_planner_heading_error_deg(current_yaw, target_bearing)` 将方位角差值规整到 [-180°, +180°]：

```c
double error = target_bearing_deg - current_yaw_deg;
while (error >  180.0) error -= 360.0;
while (error < -180.0) error += 360.0;
return error;
```

- `error > 0`：目标在车头右侧，需右转。
- `error < 0`：目标在车头左侧，需左转。
- 规整到 ±180° 是为了避免"差 350° 往右转"这种多转一圈的错误——实际只需左转 10°。

### 1.4 转向量计算（L2 CRUISE 状态）

`nav_l2_fsm.c:338` 把角度误差线性放大为转向百分比：

```c
double bearing = nav_planner_bearing_deg(sit->lat, sit->lon, s_goal_lat, s_goal_lon);
double herr    = nav_planner_heading_error_deg(sit->yaw_deg, bearing);
steer = clamp_int((int)(cfg->heading_kp * herr),
                  -cfg->heading_max_steer_pct,
                   cfg->heading_max_steer_pct);
```

- `sit->yaw_deg`：来自 IMU 积分，**bootstrap 之前这个值不在 compass 坐标系里**，所以 `herr` 计算出来是错的，转向方向随机。
- bootstrap 完成后 `sit->yaw_deg` 被对齐到 compass 坐标系，`herr` 才有意义。

---

## 二、Bootstrap 是怎么校准 IMU yaw 零点的

### 2.1 整体思路

GPS 的 COG（Course Over Ground，对地航向）在车辆直线运动且速度 > 0.3 m/s 时，能给出准确的真实 compass 朝向。Bootstrap 让车直行 3m，采集 5 个 COG 样本，确认稳定后用 GPS 告诉我们的真实朝向去覆盖 IMU 当前的 yaw 积分值。

### 2.2 COG 稳定性判断（循环均值 + 最大偏差）

`nav_l2_fsm.c:258`：

```c
// 第一步：计算 5 个样本的循环均值（circular mean）
float sum_sin = 0.0f, sum_cos = 0.0f;
for (int i = 0; i < BOOTSTRAP_COG_WINDOW; i++) {
    float r = s_cog_buf[i] * (M_PI / 180.0f);  // 转弧度
    sum_sin += sinf(r);
    sum_cos += cosf(r);
}
float mean_deg = atan2f(sum_sin, sum_cos) * (180.0f / M_PI);
if (mean_deg < 0.0f) mean_deg += 360.0f;

// 第二步：计算每个样本与均值的角度偏差，取最大值
float max_dev = 0.0f;
for (int i = 0; i < BOOTSTRAP_COG_WINDOW; i++) {
    float dev = fabsf(s_cog_buf[i] - mean_deg);
    if (dev > 180.0f) dev = 360.0f - dev;  // 处理 0°/360° 跨界
    if (dev > max_dev) max_dev = dev;
}

// 第三步：最大偏差 < 5° 则认为 COG 稳定
if (max_dev < BOOTSTRAP_COG_MAX_DEV_DEG) { /* 执行对齐 */ }
```

**为什么用循环均值而不是算术均值？**

因为角度是循环量。如果 5 个样本是 359°、1°、0°、2°、358°，算术均值 = 144°（完全错误），循环均值通过先转成单位向量再求和，得到 0°（正确）。

### 2.3 三层写入：yaw 对齐如何传播

`driver_imu_set_yaw(mean_deg)` 在 `driver_imu.c:455` 做了三件事，确保对齐结果立即对所有消费者可见：

```c
void driver_imu_set_yaw(float deg) {
    // 归一化到 [0, 360)
    while (deg >= 360.0f) deg -= 360.0f;
    while (deg <    0.0f) deg += 360.0f;

    // ① 写积分零点：imu_task 下一个 10ms 周期从这里继续积分
    s_yaw = deg;

    // ② 写发布缓存：driver_imu_get_reading() 立即返回新值（mutex 保护）
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_latest_reading.yaw_deg = deg;
    xSemaphoreGive(s_mutex);

    // ③ 写 nav_situation：L2 下个 tick 读到的 sit->yaw_deg 已是校准值
    nav_situation_update_imu(roll, pitch, deg, gz);
}
```

**三层写入的意义：**

| 写入目标 | 作用 | 不写会怎样 |
|---------|------|-----------|
| `s_yaw` | 陀螺积分起点 | 下次 imu_task 覆盖掉校准值，校准无效 |
| `s_latest_reading.yaw_deg` | driver 层缓存 | 10ms 内读 driver 还是旧值 |
| `nav_situation.yaw_deg` | L2 消费的 sit 结构体 | L2 第一个 CRUISE tick 仍用旧 yaw，转向一次错误 |

### 2.4 校准前后的数据流对比

**校准前（bootstrap 完成之前）：**

```
MPU6050 gz → imu_task 积分 → s_yaw（0° 起点，≠ 真北）
    → nav_situation.yaw_deg（IMU 坐标系）
    → fsm_cruise: herr = bearing_compass - yaw_imu  ← 坐标系不同，错误
    → steer = Kp * herr  ← 方向随机
```

**校准后（bootstrap 完成之后）：**

```
GPS COG → mean_deg（循环均值）
    → driver_imu_set_yaw(mean_deg)
        → s_yaw = mean_deg（积分起点对齐真北）
        → nav_situation.yaw_deg = mean_deg（compass 坐标系）
MPU6050 gz → imu_task 继续积分 → s_yaw += -gz*dt（从真北出发的相对变化）
    → nav_situation.yaw_deg（现在在 compass 坐标系）
    → fsm_cruise: herr = bearing_compass - yaw_compass  ← 同一坐标系，正确
    → steer = Kp * herr  ← 方向正确
```

---

## 三、修复方案：GPS COG Bootstrap

### 实现位置

| 文件 | 内容 |
|------|------|
| `main/drivers/driver_imu.c:455` | `driver_imu_set_yaw(float deg)` — 三层写入对齐 yaw |
| `main/nav/nav_l2_fsm.c:211` | `fsm_yaw_bootstrap()` — 新增 `L2_YAW_BOOTSTRAP` 状态 |
| `main/nav/nav_l2_fsm.h` | `L2_YAW_BOOTSTRAP` 枚举值 |

### Bootstrap 完整流程

```
nav_goto 触发
    ↓
[L2_YAW_BOOTSTRAP]
    ├─ 直行（steer=0，不做避障）
    ├─ 等待 GPS fix
    ├─ 记录起始坐标 (lat0, lon0)
    ├─ 每个 L2 tick（200ms）：
    │     dist = haversine(lat0, lon0, lat_now, lon_now)
    │     if dist ≥ 3m AND speed ≥ 0.3 m/s:
    │         压入 course_deg 到循环缓冲（5槽）
    │         计算循环均值 mean_deg
    │         计算最大偏差 max_dev
    │         if max_dev < 5°:
    │             driver_imu_set_yaw(mean_deg)  ← 三层写入
    │             → [L2_CRUISE]
    │
    ├─ 超时 > 10s → FAULT (bootstrap_timeout)
    └─ 行驶 > 10m 仍未稳定 → FAULT (bootstrap_no_cog)
```

### 超时与保护

| 条件 | 行为 |
|------|------|
| 超时 > 10s | → `L2_FAULT`，打印 `bootstrap_timeout` |
| 行驶 > 10m 仍未稳定 | → `L2_FAULT`，打印 `bootstrap_no_cog` |
| bootstrap 期间目标已到达 | → 直接 `L2_ARRIVED` |
| L1 紧急刹车 | 仍生效（`nav_l1_is_blocked()` 保护 throttle） |

超声波 stale 检测在 bootstrap 期间**豁免**（不因传感器未就绪而提前 FAULT）。

---

## 四、关键日志

bootstrap 完成时会打印：

```
[nav_l2] FAULT → YAW_BOOTSTRAP
[nav_l2] [bootstrap] origin=(lat, lon)
[nav_l2] [bootstrap] waiting: dist=0.5m speed=0.12 m/s
...
[nav_l2] [bootstrap] waiting: dist=3.1m speed=0.82 m/s
[driver_imu] yaw forced to 10.5° (GPS COG bootstrap)
[nav_l2] [bootstrap] yaw aligned: gps_course=10.5° yaw_pre=0.0° offset=10.5°
[nav_l2] YAW_BOOTSTRAP → CRUISE
```

进入 CRUISE 后（正常情况 error 应接近 0）：

```
[nav_l2] NAV: goal_bearing=10.5° yaw=10.6° error=-0.1° steer=0
```

---

## 五、验证方法

### Step 1 — 烧录

```bash
idf.py -p /dev/cu.usbmodem21201 flash monitor
```

### Step 2 — 保存位置并导航

```
# 在 CLI 中
> nav_pos_save home
> （把车移到 home 正南 5m，车头任意朝向）
> nav_goto home
```

### Step 3 — 验证对齐精度

bootstrap 完成时，用手机指南针 APP 测量车头当前真实 compass 朝向，与日志中 `gps_course` 对比：

- 偏差 < 10° → 对齐成功，车应朝目标正常行驶
- 偏差 > 10° → GPS COG 不够稳定，可尝试：
  - 增大 `BOOTSTRAP_MIN_DIST_M`（当前 3m，可改 5m）
  - 减小 `BOOTSTRAP_COG_MAX_DEV_DEG`（当前 5°，可改 3°）

### Step 4 — 边界场景

| 场景 | 预期行为 |
|------|----------|
| 开机朝北/东/南/西各跑一遍 | 均能正确朝向目标 |
| 屋内 GPS 信号差 | bootstrap 超时 → FAULT，不走错 |
| 目标距离 < 3m（bootstrap 超过目标） | 对齐后检查到已到达 → ARRIVED |

---

## 六、参数速查

```c
// main/nav/nav_l2_fsm.c
#define BOOTSTRAP_MIN_DIST_M       3.0f   // 开始采样的最小行驶距离
#define BOOTSTRAP_MAX_DIST_M      10.0f   // 放弃的最大行驶距离
#define BOOTSTRAP_TIMEOUT_MS   10000LL    // 硬超时（毫秒）
#define BOOTSTRAP_MIN_SPEED_MPS    0.3f   // GPS 速度门限
#define BOOTSTRAP_COG_WINDOW          5   // COG 稳定性采样窗口大小
#define BOOTSTRAP_COG_MAX_DEV_DEG  5.0f   // 最大允许方差（度）
```
