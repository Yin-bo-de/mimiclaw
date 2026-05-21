# 修复：直线行驶 yaw 持续左偏

## Context

RC 车在导航 CRUISE 模式下直线行驶时，yaw 持续向左漂移（降低），导致车辆偏离航向。从日志量化分析：

- 对齐目标后（error≈0°），yaw 以 **2-31°/s** 的速率向左漂移
- 即使 steer 修正到 30%（~1350us），仍无法阻止漂移
- 最终车辆反复撞击障碍物、翻转

## 根因分析

### 核心问题：电机电流动态干扰磁力计 → 磁力计"纠正"反而放大陀螺漂移

**证据链：**

1. **磁力计硬铁偏移极大** — `offset_x=308.20, offset_y=-140.30`（原始 LSB），换算约 283/-129 mG，与地球磁场（300-500 mG）量级相当，说明传感器处于强磁干扰环境

2. **电机/ESC 电流产生动态磁场** — 5A 电流在 5cm 距离产生 ~200 mG 磁场，占地球磁场的 40-67%。油门变化时磁场变化，导致磁力计航向偏移

3. **磁力计 LPF 滞后** — `MAG_LPF_ALPHA=0.80` 使低通滤波器时间常数很长（~0.5s），跟不上电机电流突变造成的航向跳变

4. **磁力计纠正方向错误时反而加剧漂移** — 当电机电流使磁力计读数偏向左侧时：
   - `err = mag_yaw - s_yaw` 变为负值
   - `s_yaw += 0.02 * err` 使 yaw 进一步减小（左偏）
   - 估算：-20° 磁偏移可产生 ~20°/s 的虚假左漂

5. **无 GPS COG 校验** — GPS 滤波器计算了 `course_deg`，但未用于校正 yaw

6. **heading_kp=2.0 过低** — 纯比例控制无法消除稳态误差，15° 误差仅产生 steer=30%

### 排除项

- **转向方向未反转** — `steer_reversed=true` 正确补偿了硬件；AVOID_LEFT/AVOID_RIGHT 方向验证正确
- **陀螺仪零偏** — 自动校准在启动时运行，2 dps 量级的零偏无法解释 31°/s 的漂移速率
- **舵机居中校准** — 若 steer_center_us 偏移，应产生恒定方向漂移但速率远小于观测值

## 修复方案

### Step 1: 磁力计有效性检测 + 动态信任度

**文件**: `main/drivers/driver_imu.c`

在磁力计融合逻辑中增加一致性检测：
- 计算磁力计航向变化率 `mag_rate`（连续两次 mag_yaw 的差分）
- 计算陀螺仪积分变化率 `gyro_rate`（gz 积分值）
- 当 `|mag_rate - gyro_rate| > 30°/s` 持续超过 1 秒时，降低 `MAG_CORRECTION_GAIN` 至 0.002（10x 降权）
- 一致性恢复后，逐步恢复增益

```c
// 新增静态变量
static float s_mag_prev_yaw = 0.0f;
static int   s_mag_disagree_count = 0;
static float s_mag_trust = 1.0f;  // 0.0~1.0

// 在磁力计融合代码中
float mag_rate = mag_yaw - s_mag_prev_yaw;
while (mag_rate > 180.0f) mag_rate -= 360.0f;
while (mag_rate < -180.0f) mag_rate += 360.0f;
mag_rate /= (MAG_READ_EVERY_N_CYCLES * 0.01f); // dps

float gyro_rate = -gz; // gz 已减去零偏，取反为 CW+ 约定
float rate_diff = fabsf(mag_rate - gyro_rate);

if (rate_diff > 30.0f) {
    s_mag_disagree_count++;
    if (s_mag_disagree_count > 50) { // 1s @ 50Hz
        s_mag_trust = fmaxf(0.1f, s_mag_trust - 0.02f);
    }
} else {
    s_mag_disagree_count = 0;
    s_mag_trust = fminf(1.0f, s_mag_trust + 0.005f);
}

float effective_gain = MAG_CORRECTION_GAIN * s_mag_trust;
s_yaw += effective_gain * err;
```

### Step 2: GPS COG 航向校正

**文件**: `main/drivers/driver_imu.c`

在 `imu_update()` 中直接读取 `nav_situation` 的 GPS 数据（IMU 已有 `nav_situation_update_imu()` 依赖）。
每 20 个 IMU 周期（200ms）检查一次 GPS COG：
- 条件：`gps_fix == true && speed_mps > 1.0 && sats >= 6`
- 增益：0.005（极慢，仅作为长期锚点防止磁力计+陀螺共同漂移）

```c
// 在 imu_update() 的磁力计融合代码之后
s_gps_cog_counter++;
if (s_gps_cog_counter >= 20) { // 每 200ms
    s_gps_cog_counter = 0;
    nav_situation_t gps_sit;
    nav_situation_get(&gps_sit);
    if (gps_sit.gps_fix && gps_sit.speed_mps > 1.0 && gps_sit.gps_sats >= 6) {
        float cog = gps_sit.course_deg;
        float cog_err = cog - s_yaw;
        while (cog_err > 180.0f) cog_err -= 360.0f;
        while (cog_err < -180.0f) cog_err += 360.0f;
        s_yaw += 0.005f * cog_err;
        while (s_yaw >= 360.0f) s_yaw -= 360.0f;
        while (s_yaw < 0.0f) s_yaw += 360.0f;
    }
}
```

**不需要修改 nav_l1_reflex.c 或 driver_imu.h** — 所有逻辑自包含在 driver_imu.c 内。

### Step 3: 增强 CRUISE 航向控制器

**文件**: `main/nav/nav_l2_fsm.c`, `main/nav/nav_config.h`

将纯 P 控制改为 PD 控制，增加微分项抑制漂移速率：

```c
// nav_config 新增
float heading_ki;    // 积分增益，消除稳态误差
float heading_kd;    // 微分增益，抑制漂移速率

// 默认值
.heading_kp = 2.0f,
.heading_ki = 0.1f,
.heading_kd = 0.5f,
```

在 `fsm_cruise()` 中：
```c
static float s_heading_integral = 0.0f;
static float s_prev_heading_error = 0.0f;

double herr = nav_planner_heading_error_deg(sit->yaw_deg, bearing);

// 积分项（带饱和）
s_heading_integral += herr * 0.1f; // L2 周期 100ms
s_heading_integral = clamp_float(s_heading_integral, -50.0f, 50.0f);

// 微分项
float herr_deriv = (herr - s_prev_heading_error) / 0.1f;
s_prev_heading_error = herr;

int steer = (int)(cfg->heading_kp * herr
                + cfg->heading_ki * s_heading_integral
                + cfg->heading_kd * herr_deriv);
steer = clamp_int(steer, -cfg->heading_max_steer_pct, cfg->heading_max_steer_pct);
```

### Step 4: 磁力计 LPF 参数调优

**文件**: `main/drivers/driver_imu.c`

```c
#define MAG_LPF_ALPHA           0.50f   // 从 0.80 降低，加快跟踪
#define MAG_CORRECTION_GAIN     0.03f   // 从 0.02 微增
#define MAG_READ_EVERY_N_CYCLES 2       // 保持不变
```

## 修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `main/drivers/driver_imu.c` | 磁力计有效性检测 + GPS COG 校正 + LPF 参数调整 |
| `main/nav/nav_l2_fsm.c` | PD+I 航向控制器 |
| `main/nav/nav_config.h` | 新增 heading_ki, heading_kd 字段 |
| `main/nav/nav_config.c` | 新增参数默认值 + JSON 解析 |

## 验证计划

1. **静止测试** — 车辆不动，观察 yaw 在 30 秒内漂移 < 1°
2. **直线测试** — 遥控直线行驶 30 秒，yaw 漂移 < 5°
3. **导航测试** — nav_goto_waypoint 到 30m 外目标，观察：
   - CRUISE 模式下 yaw 是否稳定
   - 磁力计信任度是否在电机启动时下降
   - GPS COG 校正是否生效（日志可观察）
   - 到达误差 < 3m
4. **日志验证** — 添加 mag_trust, gps_cog_correction 调试输出
