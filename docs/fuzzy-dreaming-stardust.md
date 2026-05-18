# HMC5883L 磁力计集成计划 —— 替换 GPS COG Bootstrap 提供真实 Compass 方向

## Context（为什么要做）

用户已准备 HMC5883L 磁力计模块，要求将当前基于 GPS COG 的 yaw 对齐方案替换为磁力计方案。磁力计能在静止状态下直接感知地磁场方向，提供绝对的 compass 航向参考，无需车辆直线行驶 3m+ 的 bootstrap 过程。

**当前方案（GPS COG Bootstrap）的局限：**
- 必须行驶 3m+ 且速度 >0.3m/s 才能采集稳定 COG
- 启动导航时有一段"盲开"的直行过程
- 室内/弱 GPS 环境下无法完成 bootstrap

**磁力计方案的优势：**
- 开机即有绝对方向，无需移动车辆
- 导航可立即进入 CRUISE 状态
- 室内/静态环境同样有效（只要校准过硬磁偏移）

---

## 根因边界条件

1. **MPU6050 仍负责 gyro 积分** — 提供高频率（100Hz）的短期航向变化，响应快、噪声低。
2. **HMC5883L 负责绝对定北** — 提供长期（0Hz 漂移）的 compass 参考，但受硬磁/软磁干扰、有噪声。
3. **两者互补融合** — gyro 负责短期动态，mag 负责长期纠偏，是标准做法。
4. **坐标系约定不变** — 0°=真北（磁北+磁偏角补偿），顺时针为正，范围 [0°, 360°)。

---

## 接线方案

HMC5883L 与 MPU6050 共用同一 I2C0 总线（I2C 天生支持多设备地址）：

```
ESP32-S3 (GPIO)    MPU6050模块      HMC5883L模块
----------------   -----------      ------------
GPIO8  (SDA)  ──── SDA    ────────  SDA
GPIO9  (SCL)  ──── SCL    ────────  SCL
3.3V          ──── VCC    ────────  VCC
GND           ──── GND    ────────  GND
                 
MPU6050 地址: 0x68（已验证）
HMC5883L 地址: 0x1E（7-bit，ADDR引脚接地）
```

**注意：**
- 大多数 MPU6050 模块已带 4.7kΩ 上拉电阻，HMC5883L 模块通常也有。
- 并联后等效上拉电阻会变小（约 2.3kΩ），但仍在 I2C 规范内，无需额外电阻。
- 若个别模块无上拉，需在 GPIO8/GPIO9 到 3.3V 之间各加 4.7kΩ 外部上拉。
- HMC5883L 的 DRDY 引脚可不接，软件轮询方式读取。

---

## 软件架构变更

### 一、HMC5883L 驱动层（`main/drivers/driver_imu.c`）

**新增内容：**

1. **寄存器定义**
   - `HMC5883L_REG_CRA` (0x00): 配置寄存器 A（采样平均数、输出速率、测量模式）
   - `HMC5883L_REG_CRB` (0x01): 配置寄存器 B（增益/量程）
   - `HMC5883L_REG_MODE` (0x02): 模式寄存器（连续/单次/idle）
   - `HMC5883L_REG_DXRA` (0x03): X轴数据高字节（输出顺序：X, Z, Y）
   - `HMC5883L_REG_STA` (0x09): 状态寄存器（bit0=RDY）
   - `HMC5883L_REG_ID_A` (0x0A): 识别寄存器 A，应读出 'H'
   - `HMC5883L_REG_ID_B` (0x0B): 识别寄存器 B，应读出 '4'
   - `HMC5883L_REG_ID_C` (0x0C): 识别寄存器 C，应读出 '3'

2. **I2C 设备句柄**
   - 新增 `static i2c_master_dev_handle_t s_mag_dev = NULL;`
   - 在 `driver_imu_init()` 中，`i2c_new_master_bus()` 创建 bus 后，用 `i2c_master_bus_add_device()` 添加 HMC5883L 设备（地址 0x1E）。
   - bus 共用 MPU6050 的 `s_i2c_bus`，无需新建。

3. **初始化流程 `hmc5883l_init()`**
   - 读取 ID 寄存器（0x0A-0x0C）验证 'H', '4', '3'，确认芯片在线。
   - 写 CRA = 0x78: 8-average, 75Hz output rate, normal measurement mode。
   - 写 CRB = 0x20: ±1.3 Ga, gain = 0.92 mG/LSB。
   - 写 MODE = 0x00: Continuous measurement mode。
   - 若初始化失败，打印警告，driver 仍继续运行（仅 MPU6050 工作，但导航 bootstrap 逻辑已被移除，需保证磁力计失败后系统仍有方向参考——这是关键风险）。

4. **读取函数 `hmc5883l_read_raw()`**
   - 读状态寄存器 STA 确认 bit0 (RDY)。
   - 从 0x03 读取 6 字节：X_H, X_L, Z_H, Z_L, Y_H, Y_L。
   - 组合成 `int16_t mag_x, mag_y, mag_z`。
   - 返回值按增益换算为 milliGauss：`* 0.92f`。

5. **Heading 计算 `mag_compute_heading()`**
   - 硬磁补偿：`mx = (mag_x - offset_x) * scale_x`, `my = (mag_y - offset_y) * scale_y`。
   - 轴方向补偿：若模块安装方向导致 X/Y 轴与车头坐标系不一致，通过 `mag_x_inverted`、`mag_y_inverted` 配置反转。
   - `heading = atan2f(my, mx) * RAD_TO_DEG`。
   - 归一化到 [0, 360)。
   - 磁偏角补偿：`heading += mag_declination_deg`（中国大部分地区约 -5° ~ -10°，西偏为负）。
   - 再次归一化。

6. **互补滤波融合（在 `imu_update()` 中）**
   当前 `imu_update()` 已用互补滤波融合 gyro + accel 做 roll/pitch，yaw 仅靠 gyro 积分。
   修改后：
   ```c
   // 1. Gyro predict（保持原有逻辑）
   s_yaw -= gz * dt;
   while (s_yaw >= 360.0f) s_yaw -= 360.0f;
   while (s_yaw <    0.0f) s_yaw += 360.0f;

   // 2. Mag correction（新增）
   if (s_mag_available) {
       float mag_yaw = mag_compute_heading();
       // 低通滤波磁力计读数，抑制高频噪声
       s_mag_yaw_lpf = MAG_LPF_ALPHA * s_mag_yaw_lpf + (1.0f - MAG_LPF_ALPHA) * mag_yaw;

       // 计算 gyro 预测与 mag 测量的误差（处理 0°/360° 跨界）
       float err = s_mag_yaw_lpf - s_yaw;
       while (err >  180.0f) err -= 360.0f;
       while (err < -180.0f) err += 360.0f;

       // 缓慢校正 gyro 积分漂移（2% 每 tick @100Hz ≈ 0.5s 时间常数）
       s_yaw += MAG_CORRECTION_GAIN * err;
       while (s_yaw >= 360.0f) s_yaw -= 360.0f;
       while (s_yaw <    0.0f) s_yaw += 360.0f;
   }
   ```
   参数：
   - `MAG_LPF_ALPHA = 0.80f` — 磁力计低通滤波系数，越大越平滑。
   - `MAG_CORRECTION_GAIN = 0.02f` — 漂移校正增益，越大纠偏越快但噪声影响越大。

7. **磁力计校准 `driver_imu_calibrate_mag()`**
   - 新增函数，CLI 可调用。
   - 流程：
     1. 提示用户将车水平放置，原地缓慢旋转 360°（约 10-20 秒）。
     2. 每 50ms 读取一次 mag_x, mag_y。
     3. 记录 `min_x, max_x, min_y, max_y`。
     4. 计算硬磁偏移：`offset_x = (max_x + min_x) / 2`, `offset_y = (max_y + min_y) / 2`。
     5. 保存到 `sensors.json` 的 `magnetometer` 字段。
   - 若未校准（offset 为 0），仍可使用，但在强干扰环境（如靠近马达/电池）下精度会下降。

### 二、配置层（`main/drivers/sensor_config.c/h`）

**`sensor_config.h` 新增：**
```c
typedef struct {
    int i2c_port;          /* 共用 MPU6050 的 I2C 总线，默认 0 */
    uint8_t address;       /* 默认 0x1E */
    bool enabled;          /* 是否启用磁力计 */
    float declination_deg; /* 磁偏角，中国约 -5 ~ -10 */
    bool x_inverted;       /* X 轴是否反向 */
    bool y_inverted;       /* Y 轴是否反向 */
    float offset_x;        /* 硬磁校准偏移 X */
    float offset_y;        /* 硬磁校准偏移 Y */
    bool loaded;
} magnetometer_config_t;
```

**`sensor_config.c` 新增：**
- 默认值：`i2c_port=0`, `address=0x1E`, `enabled=true`, `declination_deg=0`, `x_inverted=false`, `y_inverted=false`, `offset_x=0`, `offset_y=0`。
- 从 `sensors.json` 加载/保存 `magnetometer` 字段。
- 新增 `sensor_config_get_magnetometer()` 和 `sensor_config_save_mag_offset()`。

### 三、导航层（`main/nav/nav_l2_fsm.c/h`）

**移除 YAW_BOOTSTRAP：**
1. `nav_l2_fsm.h` — 保留 `L2_YAW_BOOTSTRAP` 枚举值（向后兼容，避免其他代码编译错误），但注释标记为 deprecated / unused。
2. `nav_l2_fsm.c`：
   - `l2_task()` 启动时：`enter_state(L2_CRUISE)` 替代 `enter_state(L2_YAW_BOOTSTRAP)`。
   - `L2_CMD_NEW_GOAL` 处理：`enter_state(L2_CRUISE)` 替代 `enter_state(L2_YAW_BOOTSTRAP)`。
   - `fsm_yaw_bootstrap()` 函数可保留（不做任何行为，仅打印日志 `yaw bootstrap deprecated, magnetometer active`）或删除。为代码简洁，建议删除。
   - `bootstrap_reset()` 可删除。
   - 移除所有 bootstrap 相关静态变量（`s_boot_start_lat`, `s_cog_buf` 等）。
   - Sensor staleness guard 不再豁免 `L2_YAW_BOOTSTRAP`（该状态已不存在）。

**结果：** 用户执行 `nav_goto home` 后，车辆立即进入 CRUISE 状态并开始根据 bearing error 转向，无需先直行 3m。

### 四、CLI 层（`main/cli/serial_cli.c`）

新增 CLI 命令：
- `mag_status` — 打印磁力计当前状态：是否在线、原始 XYZ、计算 heading、当前偏移值。
- `mag_cal` — 启动硬磁校准流程，提示用户旋转车辆 360°，完成后保存偏移。
- `mag_decl <deg>` — 设置磁偏角（如 `mag_decl -7.5`），保存到 sensors.json。

检查 `serial_cli.c` 中现有 CLI 命令注册方式，复用现有模式添加新命令。

---

## 关键参数

```c
/* main/drivers/driver_imu.c */
#define HMC5883L_ADDR               0x1E
#define HMC5883L_GAIN_MG_PER_LSB    0.92f   /* ±1.3 Ga, CRB=0x20 */
#define MAG_LPF_ALPHA               0.80f   /* 磁力计低通滤波 */
#define MAG_CORRECTION_GAIN         0.02f   /* gyro 漂移校正增益 */
#define MAG_READ_EVERY_N_CYCLES     2       /* 100Hz IMU 中每 2 周期读一次 mag = 50Hz */
```

**磁偏角参考（中国主要城市）：**
| 城市 | 磁偏角（2024 approx）|
|------|---------------------|
| 北京 | -7.0° |
| 上海 | -6.0° |
| 广州 | -3.5° |
| 成都 | -2.0° |
| 西安 | -4.0° |

用户可通过 `mag_decl` 命令设置本地磁偏角。

---

## 验证方案

### Step 1 — 编译与烧录

```bash
idf.py build
idf.py -p /dev/cu.usbmodem21201 flash monitor
```

### Step 2 — 验证 HMC5883L 在线

在 CLI 中执行：
```
> mag_status
```

预期输出：
```
[driver_imu] HMC5883L online (ID=H43)
[driver_imu] Mag raw: X=123 Y=-45 Z=678
[driver_imu] Mag heading: 15.3° (decl=0.0°)
[driver_imu] Mag offset: X=0.0 Y=0.0
```

### Step 3 — 验证方向基本正确

1. 车头朝北（用手机指南针确认）。
2. 执行 `mag_status`，确认 heading ≈ 0°（若偏差大，说明轴反转或磁偏角未设）。
3. 车头朝东，确认 heading ≈ 90°。
4. 车头朝南，确认 heading ≈ 180°。
5. 车头朝西，确认 heading ≈ 270°。

若方向相反（如朝北显示 180°），通过 `sensors.json` 设置 `x_inverted: true` 或 `y_inverted: true` 修正。

### Step 4 — 硬磁校准

1. 执行 `mag_cal`。
2. 按提示将车水平放置，原地缓慢旋转 360°。
3. 校准完成后执行 `mag_status`，确认 offset 已更新。
4. 再次测试四个方向，精度应提升到 ±5° 以内。

### Step 5 — 导航验证

```
> nav_pos_save home
> （把车移到 home 正南方 5m，车头任意朝向）
> nav_goto home
```

预期行为（对比旧方案）：
- **旧方案**：先直行 3m+ bootstrap，然后才转向目标。
- **新方案**：立即根据 bearing error 转向，朝北行驶。

日志应直接出现：
```
[nav_l2] FAULT → CRUISE
[nav_l2] 决策：前进 (throttle=35%)
[nav_l2] NAV: goal_bearing=0.0° yaw=175.2° error=-175.2° steer=-100
...（车辆掉头后）...
[nav_l2] NAV: goal_bearing=0.0° yaw=2.1° error=-2.1° steer=-5
```

### Step 6 — 长期漂移验证

让车静止 5 分钟，每隔 30 秒执行 `mag_status`：
- yaw 应稳定在 ±2° 以内（磁力计持续纠正 gyro drift）。
- 若漂移 > 10°，增大 `MAG_CORRECTION_GAIN`（如 0.05）。

---

## 风险与回退

| 风险 | 缓解措施 |
|------|----------|
| HMC5883L 模块实际是 QMC5883L（寄存器不兼容） | 初始化时读 ID 寄存器验证（'H','4','3'），失败则打印明确错误提示用户更换模块 |
| 车辆周围强磁场干扰（马达、电池、金属）导致 heading 跳变 | 硬磁校准 + 低通滤波 + 陀螺仪主导短期动态；若跳变严重，可降低 `MAG_CORRECTION_GAIN` |
| I2C 总线带两个设备后通信不稳定 | 可降低 I2C 频率到 100kHz；或加外部 4.7kΩ 上拉 |
| 磁偏角未知导致固定偏移 | 导航 feedback loop 对固定偏移有天然抑制能力（积分效应），且用户可通过 `mag_decl` 微调 |
| 用户未校准硬磁偏移 | offset 默认 0，可用但精度下降；CLI 提供 `mag_cal` 命令 |

---

## 关键文件修改清单

| 文件 | 修改内容 |
|------|----------|
| `main/drivers/sensor_config.h` | 新增 `magnetometer_config_t` 结构体；新增 `sensor_config_get_magnetometer()` 声明 |
| `main/drivers/sensor_config.c` | 新增磁力计默认配置、JSON 加载/保存逻辑 |
| `main/drivers/driver_imu.h` | 新增 `driver_imu_calibrate_mag()`、`driver_imu_get_mag_status()` 声明 |
| `main/drivers/driver_imu.c` | **核心修改**：HMC5883L 寄存器定义、I2C 设备添加、初始化、读取、heading 计算、互补滤波融合、校准函数 |
| `main/nav/nav_l2_fsm.h` | `L2_YAW_BOOTSTRAP` 标记为 deprecated |
| `main/nav/nav_l2_fsm.c` | 移除 bootstrap 进入逻辑，task 启动和 NEW_GOAL 直接进入 `L2_CRUISE`；删除 bootstrap 相关函数和变量 |
| `main/cli/serial_cli.c` | 新增 `mag_status`、`mag_cal`、`mag_decl` CLI 命令 |
| `docs/compass-adjust.md` | 追加磁力计方案说明（可选，执行后再更新文档） |
