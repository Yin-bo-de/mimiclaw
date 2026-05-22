# MimiClaw ESP32-S3 引脚接线总览

> 本文档汇总项目中所有硬件模块的引脚分配，基于 `main/tools/gpio_policy.h`、`main/drivers/sensor_config.c`、`main/tools/tool_pwm.c` 中的实际代码定义。所有引脚均为 **ESP32-S3** GPIO。

---

## 一、引脚分配总表

| 功能模块 | 信号 | GPIO | 方向 | 备注 |
|---------|------|------|------|------|
| **I2C0 (MPU6050 + HMC5883L)** | SDA | 8 | 双向 | 4.7kΩ 上拉（模块自带） |
| | SCL | 9 | 输出 | 4.7kΩ 上拉（模块自带） |
| **超声波 左 (HC-SR04)** | TRIG | 10 | 输出 | |
| | ECHO | 12 | 输入 | |
| **超声波 前 (HC-SR04)** | TRIG | 13 | 输出 | |
| | ECHO | 14 | 输入 | |
| **超声波 右 (HC-SR04)** | TRIG | 15 | 输出 | |
| | ECHO | 16 | 输入 | |
| **GPS (NEO-6M)** | RX | 17 | 输入 | 接 GPS 模块 TX |
| | TX | 18 | 输出 | 接 GPS 模块 RX |
| **舵机 (Steer)** | PWM | 11 | 输出 | 50Hz, 1000~2000us |
| **电调/油门 (ESC)** | PWM | 21 | 输出 | 50Hz, 1000~2000us |

---

## 二、分模块接线图

### 2.1 I2C0 总线 — MPU6050 + HMC5883L

```
ESP32-S3          MPU6050 模块        HMC5883L 模块
--------          -----------         ------------
GPIO8  (SDA)  ────┬── SDA    ────────┬── SDA
GPIO9  (SCL)  ────┬── SCL    ────────┬── SCL
3.3V          ────┬── VCC    ────────┬── VCC
GND           ────┴── GND    ────────┴── GND

MPU6050 I2C 地址: 0x68（已验证）
HMC5883L I2C 地址: 0x1E（ADDR 引脚接地）
总线频率: 400 kHz
```

**注意：**
- 两模块共用同一 I2C 总线，I2C 多设备通过不同地址区分。
- 大多数模块自带 4.7kΩ 上拉电阻，并联后等效约 2.3kΩ，仍在 I2C 规范内，通常无需额外上拉。
- 若个别模块无上拉，可在 GPIO8/GPIO9 到 3.3V 之间各加 4.7kΩ 外部上拉电阻。

---

### 2.2 HC-SR04 超声波传感器 × 3

```
ESP32-S3          左侧 HC-SR04       前方 HC-SR04       右侧 HC-SR04
--------          --------------     --------------     --------------
GPIO10 (TRIG) ──── TRIG
GPIO12 (ECHO) ──── ECHO
                  VCC ─── 3.3V/5V
                  GND ─── GND

GPIO13 (TRIG) ───────────────────── TRIG
GPIO14 (ECHO) ───────────────────── ECHO
                                     VCC ─── 3.3V/5V
                                     GND ─── GND

GPIO15 (TRIG) ───────────────────────────────────────── TRIG
GPIO16 (ECHO) ───────────────────────────────────────── ECHO
                                                          VCC ─── 3.3V/5V
                                                          GND ─── GND
```

**注意：**
- HC-SR04 逻辑电平为 5V，但 ECHO 输出高电平约 5V。ESP32-S3 GPIO 仅耐压 3.3V，**必须在 ECHO 引脚串联 1kΩ 限流电阻**或使用电平转换器。
- 若模块不支持 3.3V 供电，需使用 5V 供电，但 ECHO 到 ESP32 仍需限流保护。

---

### 2.3 NEO-6M GPS 模块

```
ESP32-S3          NEO-6M 模块
--------          -----------
GPIO17 (RX)  ──── TX (模块发送)
GPIO18 (TX)  ──── RX (模块接收)
3.3V         ──── VCC
GND          ──── GND

UART 端口: UART1
波特率: 9600 bps
```

**注意：**
- ESP32 RX 接 GPS TX，ESP32 TX 接 GPS RX（交叉连接）。
- NEO-6M 某些版本为 5V 供电，但逻辑电平仍为 3.3V TTL，可直接与 ESP32 连接。

---

### 2.4 RC 舵机 + 电调 (ESC)

```
ESP32-S3          舵机 (Steer)        电调/ESC (Throttle)
--------          ------------        --------------------
GPIO4  (PWM)  ──── 信号线
GPIO5  (PWM)  ─────────────────────── 信号线
3.3V/5V       ────┬── 电源 (+)
                  └── 电源 (+)
GND           ────┴── 地线 (-)
                  └── 地线 (-)

PWM 频率: 50 Hz (周期 20ms)
舵机脉宽: 1000us (左满舵) ~ 1500us (中位) ~ 2000us (右满舵)
油门脉宽: 1000us (全退) ~ 1500us (中性/停) ~ 2000us (全进)
```

**注意：**
- 舵机和 ESC 的信号线只需 GPIO 输出，但**必须共地**（GND 相连）。
- 若舵机/ESC 电流较大，建议独立 5V 供电，不可从 ESP32 3.3V 引脚取电。
- 实际引脚可通过 `/spiffs/config/rc.json` 配置覆盖，代码默认值为 GPIO4/GPIO5。

---

## 三、ESP32-S3 引脚使用全景

```
GPIO 0   ── BOOT 按键 / 下载模式（系统保留，勿接外设）
GPIO 1   ── 空闲 ✅
GPIO 2   ── 空闲 ✅
GPIO 3   ── 空闲 ✅
GPIO 4   ── 空闲 ✅
GPIO 5   ── 空闲 ✅
GPIO 11  ── 舵机 PWM (Steer)          [已用]
GPIO 21  ── 油门 PWM (Throttle/ESC)   [已用]
GPIO 6   ── 空闲 ✅
GPIO 7   ── 空闲 ✅
GPIO 8   ── I2C0 SDA (MPU6050+HMC5883L) [已用]
GPIO 9   ── I2C0 SCL (MPU6050+HMC5883L) [已用]
GPIO 10  ── 超声波左 TRIG             [已用]
GPIO 11  ── 舵机 PWM (Steer)          [已用]
GPIO 12  ── 超声波左 ECHO             [已用]
GPIO 13  ── 超声波前 TRIG             [已用]
GPIO 14  ── 超声波前 ECHO             [已用]
GPIO 15  ── 超声波右 TRIG             [已用]
GPIO 16  ── 超声波右 ECHO             [已用]
GPIO 17  ── UART1 RX (GPS TX)         [已用]
GPIO 18  ── UART1 TX (GPS RX)         [已用]
GPIO 19  ── USB D- (系统保留)
GPIO 20  ── USB D+ (系统保留)
GPIO 21  ── 油门 PWM (Throttle/ESC)   [已用]
GPIO 26  ── SPI Flash / PSRAM（系统保留，勿用）
...      ── Flash/PSRAM 专用引脚（26~37 系统保留）
GPIO 38  ── 空闲 ✅（部分封装可用）
GPIO 46  ── 空闲 ✅（部分封装可用）
```

---

## 四、可用空闲引脚列表

以下引脚在项目允许列表中且**当前未被占用**，可用于未来扩展：

| GPIO | 推荐用途 | 说明 |
|------|---------|------|
| GPIO 1 | 通用 I/O、按键 | |
| GPIO 2 | 通用 I/O、按键 | |
| GPIO 3 | 通用 I/O、按键 | |
| GPIO 4 | 通用 I/O | 旧文档误标为舵机，实际代码未使用 |
| GPIO 5 | 通用 I/O | 旧文档误标为油门，实际代码未使用 |
| GPIO 6 | 通用 I/O、WS2812 LED | 带 RTC 功能 |
| GPIO 7 | 通用 I/O | |
| GPIO 38 | 通用 I/O | 部分封装可用 |
| GPIO 46 | 通用 I/O | 部分封装可用 |

---

## 五、软件配置入口

各模块引脚可在 SPIFFS 配置文件 `/spiffs/config/sensors.json` 中自定义：

```json
{
  "version": 1,
  "ultrasonic": {
    "left":  {"trig_gpio": 10, "echo_gpio": 12},
    "front": {"trig_gpio": 13, "echo_gpio": 14},
    "right": {"trig_gpio": 15, "echo_gpio": 16}
  },
  "imu": {
    "i2c_port": 0,
    "sda_gpio": 8,
    "scl_gpio": 9,
    "address": "0x68"
  },
  "gps": {
    "uart_port": 1,
    "rx_gpio": 17,
    "tx_gpio": 18,
    "baudrate": 9600
  },
  "magnetometer": {
    "i2c_port": 0,
    "address": "0x1E",
    "enabled": true
  }
}
```

RC 引脚在 `/spiffs/config/rc.json` 中配置：

```json
{
  "steer_gpio": 4,
  "throttle_gpio": 5,
  "steer_center_us": 1500,
  "steer_min_us": 1000,
  "steer_max_us": 2000,
  "throttle_neutral_us": 1500,
  "throttle_forward_us": 2000,
  "throttle_reverse_us": 1000,
  "steer_reversed": false,
  "throttle_reversed": false
}
```

---

## 六、烧录与调试接口

ESP32-S3 有两个 USB-C 接口：

| 接口 | 功能 | 串口设备 (macOS) |
|------|------|-----------------|
| **USB (JTAG)** | 固件烧录 (`idf.py flash`) | `/dev/cu.usbmodemXXXXX` |
| **COM (UART)** | 串口 CLI REPL (`idf.py monitor`) | `/dev/cu.usbserial-XXXXX` |

烧录命令：
```bash
idf.py -p /dev/cu.usbmodem21201 flash monitor
```

---

*文档版本: 2026-05-17 | 基于 commit a70451a*
