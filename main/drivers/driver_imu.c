#include "drivers/driver_imu.h"
#include "drivers/sensor_config.h"
#include "nav/nav_situation.h"
#include "mimi_config.h"
#include "tools/gpio_policy.h"

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"

#include <string.h>
#include <math.h>

static const char *TAG = "driver_imu";

/* MPU6050 registers */
#define MPU6050_REG_SELF_TEST_X     0x0D
#define MPU6050_REG_SELF_TEST_Y     0x0E
#define MPU6050_REG_SELF_TEST_Z     0x0F
#define MPU6050_REG_SELF_TEST_A     0x10
#define MPU6050_REG_SMPLRT_DIV      0x19
#define MPU6050_REG_CONFIG          0x1A
#define MPU6050_REG_GYRO_CONFIG     0x1B
#define MPU6050_REG_ACCEL_CONFIG    0x1C
#define MPU6050_REG_FIFO_EN         0x23
#define MPU6050_REG_I2C_MST_CTRL    0x24
#define MPU6050_REG_I2C_SLV0_ADDR   0x25
#define MPU6050_REG_I2C_SLV0_REG    0x26
#define MPU6050_REG_I2C_SLV0_CTRL   0x27
#define MPU6050_REG_I2C_SLV1_ADDR   0x28
#define MPU6050_REG_I2C_SLV1_REG    0x29
#define MPU6050_REG_I2C_SLV1_CTRL   0x2A
#define MPU6050_REG_I2C_SLV2_ADDR   0x2B
#define MPU6050_REG_I2C_SLV2_REG    0x2C
#define MPU6050_REG_I2C_SLV2_CTRL   0x2D
#define MPU6050_REG_I2C_SLV3_ADDR   0x2E
#define MPU6050_REG_I2C_SLV3_REG    0x2F
#define MPU6050_REG_I2C_SLV3_CTRL   0x30
#define MPU6050_REG_I2C_SLV4_ADDR   0x31
#define MPU6050_REG_I2C_SLV4_REG    0x32
#define MPU6050_REG_I2C_SLV4_DO     0x33
#define MPU6050_REG_I2C_SLV4_CTRL   0x34
#define MPU6050_REG_I2C_MST_DELAY_CTRL 0x36
#define MPU6050_REG_SIGNAL_PATH_RESET 0x37
#define MPU6050_REG_USER_CTRL       0x6A
#define MPU6050_REG_PWR_MGMT_1      0x6B
#define MPU6050_REG_PWR_MGMT_2      0x6C
#define MPU6050_REG_FIFO_COUNTH     0x72
#define MPU6050_REG_FIFO_COUNTL     0x73
#define MPU6050_REG_FIFO_R_W        0x74
#define MPU6050_REG_WHO_AM_I        0x75

#define MPU6050_REG_ACCEL_XOUT_H    0x3B
#define MPU6050_REG_ACCEL_XOUT_L    0x3C
#define MPU6050_REG_ACCEL_YOUT_H    0x3D
#define MPU6050_REG_ACCEL_YOUT_L    0x3E
#define MPU6050_REG_ACCEL_ZOUT_H    0x3F
#define MPU6050_REG_ACCEL_ZOUT_L    0x40
#define MPU6050_REG_TEMP_OUT_H      0x41
#define MPU6050_REG_TEMP_OUT_L      0x42
#define MPU6050_REG_GYRO_XOUT_H     0x43
#define MPU6050_REG_GYRO_XOUT_L     0x44
#define MPU6050_REG_GYRO_YOUT_H     0x45
#define MPU6050_REG_GYRO_YOUT_L     0x46
#define MPU6050_REG_GYRO_ZOUT_H     0x47
#define MPU6050_REG_GYRO_ZOUT_L     0x48

/* MPU6050 configuration values */
#define MPU6050_DEVICE_ID           0x68

/* PWR_MGMT_1 bits */
#define PWR1_DEVICE_RESET           (1 << 7)
#define PWR1_SLEEP                  (1 << 6)
#define PWR1_CYCLE                  (1 << 5)
#define PWR1_TEMP_DIS               (1 << 3)
#define PWR1_CLKSEL_INTERNAL_8MHZ   0
#define PWR1_CLKSEL_XGYRO           1
#define PWR1_CLKSEL_YGYRO           2
#define PWR1_CLKSEL_ZGYRO           3

/* GYRO_CONFIG bits */
#define GYRO_FS_SEL_250DPS          (0 << 3)
#define GYRO_FS_SEL_500DPS          (1 << 3)
#define GYRO_FS_SEL_1000DPS         (2 << 3)
#define GYRO_FS_SEL_2000DPS         (3 << 3)

/* ACCEL_CONFIG bits */
#define ACCEL_FS_SEL_2G             (0 << 3)
#define ACCEL_FS_SEL_4G             (1 << 3)
#define ACCEL_FS_SEL_8G             (2 << 3)
#define ACCEL_FS_SEL_16G            (3 << 3)

/* Scale factors */
#define GYRO_SCALE_250DPS           131.0f  /* LSB/(dps) */
#define ACCEL_SCALE_2G              16384.0f /* LSB/g */
#define GRAVITY_MPS2                9.80665f

/* Complementary filter gain */
#define COMP_FILTER_ALPHA           0.98f

/* State variables */
static bool s_initialized = false;
static bool s_running = false;
static TaskHandle_t s_task_handle = NULL;
static SemaphoreHandle_t s_mutex = NULL;
static imu_reading_t s_latest_reading;

/* Current state for complementary filter */
static float s_roll = 0.0f;
static float s_pitch = 0.0f;
static float s_yaw = 0.0f;
static int64_t s_last_update_us = 0;

/* Gyro bias (from config) */
static float s_gyro_bias_dps[3] = {0.0f, 0.0f, 0.0f};

/* HMC5883L registers */
#define HMC5883L_REG_CRA            0x00
#define HMC5883L_REG_CRB            0x01
#define HMC5883L_REG_MODE           0x02
#define HMC5883L_REG_DXRA           0x03
#define HMC5883L_REG_DXRB           0x04
#define HMC5883L_REG_DZRA           0x05
#define HMC5883L_REG_DZRB           0x06
#define HMC5883L_REG_DYRA           0x07
#define HMC5883L_REG_DYRB           0x08
#define HMC5883L_REG_STA            0x09
#define HMC5883L_REG_ID_A           0x0A
#define HMC5883L_REG_ID_B           0x0B
#define HMC5883L_REG_ID_C           0x0C

#define HMC5883L_ID_A_VAL           'H'
#define HMC5883L_ID_B_VAL           '4'
#define HMC5883L_ID_C_VAL           '3'

#define HMC5883L_GAIN_MG_PER_LSB    0.92f

/* Magnetometer fusion parameters */
#define MAG_LPF_ALPHA               0.80f
#define MAG_CORRECTION_GAIN         0.02f
#define MAG_READ_EVERY_N_CYCLES     2

/* I2C handles */
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_i2c_dev = NULL;
static i2c_master_dev_handle_t s_mag_dev = NULL;

/* Magnetometer state */
static bool s_mag_available = false;
static float s_mag_yaw_lpf = 0.0f;
static int s_mag_read_counter = 0;

/* I2C utility functions */
static esp_err_t i2c_write_reg(uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_transmit(s_i2c_dev, write_buf, 2, -1);
}

static esp_err_t i2c_read_reg(uint8_t reg_addr, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_i2c_dev, &reg_addr, 1, data, len, -1);
}

/* MPU6050 initialization */
static esp_err_t mpu6050_init(const imu_config_t *config)
{
    esp_err_t ret;

    /* Verify device ID */
    uint8_t who_am_i;
    ret = i2c_read_reg(MPU6050_REG_WHO_AM_I, &who_am_i, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WHO_AM_I: %s", esp_err_to_name(ret));
        return ret;
    }
    if (who_am_i != MPU6050_DEVICE_ID) {
        ESP_LOGE(TAG, "WHO_AM_I mismatch: expected 0x%02x, got 0x%02x", MPU6050_DEVICE_ID, who_am_i);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "MPU6050 detected (WHO_AM_I=0x%02x)", who_am_i);

    /* Reset device */
    ret = i2c_write_reg(MPU6050_REG_PWR_MGMT_1, PWR1_DEVICE_RESET);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Wake up and set clock source to X gyro */
    ret = i2c_write_reg(MPU6050_REG_PWR_MGMT_1, PWR1_CLKSEL_XGYRO);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Set sample rate divider: 1000Hz / (1 + 9) = 100Hz */
    uint8_t smplrt_div = (1000 / config->sample_hz) - 1;
    ret = i2c_write_reg(MPU6050_REG_SMPLRT_DIV, smplrt_div);
    if (ret != ESP_OK) return ret;

    /* Set config (low-pass filter) */
    ret = i2c_write_reg(MPU6050_REG_CONFIG, 0x03); /* DLPF 44Hz */
    if (ret != ESP_OK) return ret;

    /* Set gyro config: ±250 dps */
    ret = i2c_write_reg(MPU6050_REG_GYRO_CONFIG, GYRO_FS_SEL_250DPS);
    if (ret != ESP_OK) return ret;

    /* Set accel config: ±2g */
    ret = i2c_write_reg(MPU6050_REG_ACCEL_CONFIG, ACCEL_FS_SEL_2G);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "MPU6050 initialized: %dHz sample rate", config->sample_hz);
    return ESP_OK;
}

/* Read raw sensor data */
static esp_err_t mpu6050_read_raw(const imu_config_t *config, int16_t *accel, int16_t *gyro)
{
    uint8_t data[14];
    esp_err_t ret = i2c_read_reg(MPU6050_REG_ACCEL_XOUT_H, data, 14);
    if (ret != ESP_OK) return ret;

    accel[0] = (int16_t)((data[0] << 8) | data[1]);
    accel[1] = (int16_t)((data[2] << 8) | data[3]);
    accel[2] = (int16_t)((data[4] << 8) | data[5]);

    gyro[0] = (int16_t)((data[8] << 8) | data[9]);
    gyro[1] = (int16_t)((data[10] << 8) | data[11]);
    gyro[2] = (int16_t)((data[12] << 8) | data[13]);

    return ESP_OK;
}

/* HMC5883L I2C utilities */
static esp_err_t i2c_mag_write_reg(uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_transmit(s_mag_dev, write_buf, 2, -1);
}

static esp_err_t i2c_mag_read_reg(uint8_t reg_addr, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_mag_dev, &reg_addr, 1, data, len, -1);
}

/* HMC5883L initialization */
static esp_err_t hmc5883l_init(const magnetometer_config_t *config)
{
    if (!config || !config->enabled) {
        ESP_LOGW(TAG, "Magnetometer disabled in config");
        return ESP_ERR_INVALID_STATE;
    }

    /* Verify ID registers */
    uint8_t id[3];
    esp_err_t ret = i2c_mag_read_reg(HMC5883L_REG_ID_A, id, 3);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMC5883L ID read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (id[0] != HMC5883L_ID_A_VAL || id[1] != HMC5883L_ID_B_VAL || id[2] != HMC5883L_ID_C_VAL) {
        ESP_LOGE(TAG, "HMC5883L ID mismatch: expected H43, got %c%c%c", id[0], id[1], id[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "HMC5883L detected (ID=%c%c%c)", id[0], id[1], id[2]);

    /* CRA: 8-average, 75Hz, normal measurement mode */
    ret = i2c_mag_write_reg(HMC5883L_REG_CRA, 0x78);
    if (ret != ESP_OK) return ret;

    /* CRB: ±1.3 Ga, gain = 0.92 mG/LSB */
    ret = i2c_mag_write_reg(HMC5883L_REG_CRB, 0x20);
    if (ret != ESP_OK) return ret;

    /* Mode: continuous measurement */
    ret = i2c_mag_write_reg(HMC5883L_REG_MODE, 0x00);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "HMC5883L initialized: 75Hz, ±1.3Ga");
    return ESP_OK;
}

/* Read HMC5883L raw magnetometer data */
static esp_err_t hmc5883l_read_raw(int16_t *mag_x, int16_t *mag_y, int16_t *mag_z)
{
    uint8_t data[6];
    esp_err_t ret = i2c_mag_read_reg(HMC5883L_REG_DXRA, data, 6);
    if (ret != ESP_OK) return ret;

    *mag_x = (int16_t)((data[0] << 8) | data[1]);
    *mag_z = (int16_t)((data[2] << 8) | data[3]);
    *mag_y = (int16_t)((data[4] << 8) | data[5]);

    return ESP_OK;
}

/* Compute compass heading from magnetometer readings */
static float mag_compute_heading(int16_t raw_x, int16_t raw_y, const magnetometer_config_t *config)
{
    float mx = (float)raw_x;
    float my = (float)raw_y;

    /* Hard-iron offset compensation */
    mx -= config->offset_x;
    my -= config->offset_y;

    /* Axis inversion (module may be mounted rotated) */
    if (config->x_inverted) mx = -mx;
    if (config->y_inverted) my = -my;

    /* Heading: atan2(Y, X) gives 0=North, CW+ when X=forward, Y=right */
    float heading = atan2f(my, mx) * (180.0f / (float)M_PI);
    if (heading < 0.0f) heading += 360.0f;

    /* Magnetic declination: convert magnetic north to true north */
    heading += config->declination_deg;
    if (heading >= 360.0f) heading -= 360.0f;
    if (heading < 0.0f) heading += 360.0f;

    /* Install angle offset: align sensor X-axis with vehicle heading */
    heading += config->heading_offset_deg;
    if (heading >= 360.0f) heading -= 360.0f;
    if (heading < 0.0f) heading += 360.0f;

    return heading;
}

/* Convert raw data to physical units and compute attitude */
static void imu_update(const imu_config_t *config, int16_t *accel_raw, int16_t *gyro_raw, int64_t now_us)
{
    /* Convert to physical units */
    float ax = (float)accel_raw[0] / ACCEL_SCALE_2G * GRAVITY_MPS2;
    float ay = (float)accel_raw[1] / ACCEL_SCALE_2G * GRAVITY_MPS2;
    float az = (float)accel_raw[2] / ACCEL_SCALE_2G * GRAVITY_MPS2;

    float gx = (float)gyro_raw[0] / GYRO_SCALE_250DPS - s_gyro_bias_dps[0];
    float gy = (float)gyro_raw[1] / GYRO_SCALE_250DPS - s_gyro_bias_dps[1];
    float gz = (float)gyro_raw[2] / GYRO_SCALE_250DPS - s_gyro_bias_dps[2];

    /* Compute accel-based attitude (roll/pitch only) */
    float accel_roll = atan2f(ay, sqrtf(ax*ax + az*az)) * 57.2957795f;
    float accel_pitch = atan2f(-ax, sqrtf(ay*ay + az*az)) * 57.2957795f;

    /* Complementary filter */
    float dt = 0.0f;
    if (s_last_update_us > 0) {
        dt = (float)(now_us - s_last_update_us) / 1000000.0f;
        if (dt > 0.1f) dt = 0.01f; /* clamp dt */

        /* Gyro integration
         * Note: s_yaw uses minus gz because navigation bearing (0=N, CW+)
         * is clockwise-positive, while MPU6050 gz is counter-clockwise-positive.
         * Minus gz makes yaw increase clockwise, matching bearing convention.
         */
        s_roll += gx * dt;
        s_pitch += gy * dt;
        s_yaw -= gz * dt;

        /* Normalize yaw to 0..360 */
        while (s_yaw >= 360.0f) s_yaw -= 360.0f;
        while (s_yaw < 0.0f) s_yaw += 360.0f;

        /* Complementary filter: weight gyro more heavily */
        s_roll = COMP_FILTER_ALPHA * s_roll + (1.0f - COMP_FILTER_ALPHA) * accel_roll;
        s_pitch = COMP_FILTER_ALPHA * s_pitch + (1.0f - COMP_FILTER_ALPHA) * accel_pitch;

        /* Magnetometer fusion: correct gyro drift with absolute heading */
        if (s_mag_available) {
            s_mag_read_counter++;
            if (s_mag_read_counter >= MAG_READ_EVERY_N_CYCLES) {
                s_mag_read_counter = 0;
                int16_t mag_x, mag_y, mag_z;
                const magnetometer_config_t *mag_cfg = sensor_config_get_magnetometer();
                if (mag_cfg && hmc5883l_read_raw(&mag_x, &mag_y, &mag_z) == ESP_OK) {
                    float mag_yaw = mag_compute_heading(mag_x, mag_y, mag_cfg);
                    /* Low-pass filter to suppress mag noise */
                    if (s_mag_yaw_lpf == 0.0f) {
                        s_mag_yaw_lpf = mag_yaw; /* first sample */
                    } else {
                        s_mag_yaw_lpf = MAG_LPF_ALPHA * s_mag_yaw_lpf + (1.0f - MAG_LPF_ALPHA) * mag_yaw;
                    }

                    /* Slow correction of gyro drift */
                    float err = s_mag_yaw_lpf - s_yaw;
                    while (err >  180.0f) err -= 360.0f;
                    while (err < -180.0f) err += 360.0f;
                    s_yaw += MAG_CORRECTION_GAIN * err;

                    /* Normalize again */
                    while (s_yaw >= 360.0f) s_yaw -= 360.0f;
                    while (s_yaw < 0.0f) s_yaw += 360.0f;
                }
            }
        }
    } else {
        /* First update: initialize with accel */
        s_roll = accel_roll;
        s_pitch = accel_pitch;
        s_yaw = 0.0f;
        if (s_mag_available) {
            s_mag_yaw_lpf = 0.0f; /* will be set on first mag read */
        }
    }
    s_last_update_us = now_us;

    /* Update latest reading with mutex */
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_latest_reading.accel_mps2[0] = ax;
        s_latest_reading.accel_mps2[1] = ay;
        s_latest_reading.accel_mps2[2] = az;
        s_latest_reading.gyro_dps[0] = gx;
        s_latest_reading.gyro_dps[1] = gy;
        s_latest_reading.gyro_dps[2] = gz;
        s_latest_reading.roll_deg = s_roll;
        s_latest_reading.pitch_deg = s_pitch;
        s_latest_reading.yaw_deg = s_yaw;
        s_latest_reading.timestamp_us = now_us;
        s_latest_reading.valid = true;
        xSemaphoreGive(s_mutex);
    }

    /* Feed into nav_situation for L1/L2 consumption */
    nav_situation_update_imu(s_roll, s_pitch, s_yaw, gz);
}

/* Background measurement task */
static void imu_task(void *arg)
{
    const imu_config_t *config = (const imu_config_t *)arg;
    ESP_LOGI(TAG, "IMU task started");

    TickType_t last_wakeup = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / config->sample_hz);

    int16_t accel[3], gyro[3];

    while (s_running) {
        vTaskDelayUntil(&last_wakeup, period);

        esp_err_t ret = mpu6050_read_raw(config, accel, gyro);
        if (ret == ESP_OK) {
            int64_t now = esp_timer_get_time();
            imu_update(config, accel, gyro, now);
        }
    }

    ESP_LOGI(TAG, "IMU task stopped");
    vTaskDelete(NULL);
}

/* Public API */
esp_err_t driver_imu_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    const imu_config_t *config = sensor_config_get_imu();
    if (!config) {
        ESP_LOGE(TAG, "sensor config not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Validate GPIO pins */
    if (!gpio_policy_pin_is_allowed(config->sda_gpio)) {
        ESP_LOGE(TAG, "SDA GPIO %d not allowed", config->sda_gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (!gpio_policy_pin_is_allowed(config->scl_gpio)) {
        ESP_LOGE(TAG, "SCL GPIO %d not allowed", config->scl_gpio);
        return ESP_ERR_INVALID_ARG;
    }

    /* Copy gyro bias from config */
    s_gyro_bias_dps[0] = config->gyro_bias_dps[0];
    s_gyro_bias_dps[1] = config->gyro_bias_dps[1];
    s_gyro_bias_dps[2] = config->gyro_bias_dps[2];

    /* Initialize I2C bus */
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = config->i2c_port,
        .scl_io_num = config->scl_gpio,
        .sda_io_num = config->sda_gpio,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&i2c_bus_config, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Add MPU6050 device to I2C bus */
    i2c_device_config_t i2c_dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->address,
        .scl_speed_hz = config->freq_hz,
    };
    ret = i2c_master_bus_add_device(s_i2c_bus, &i2c_dev_config, &s_i2c_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        return ret;
    }

    /* Add HMC5883L device to same I2C bus */
    const magnetometer_config_t *mag_cfg = sensor_config_get_magnetometer();
    if (mag_cfg && mag_cfg->enabled) {
        i2c_device_config_t mag_dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = mag_cfg->address,
            .scl_speed_hz = config->freq_hz,
        };
        esp_err_t mag_ret = i2c_master_bus_add_device(s_i2c_bus, &mag_dev_config, &s_mag_dev);
        if (mag_ret == ESP_OK) {
            mag_ret = hmc5883l_init(mag_cfg);
            if (mag_ret == ESP_OK) {
                s_mag_available = true;
                ESP_LOGI(TAG, "Magnetometer ready, declination=%.1f°, offset=(%.1f,%.1f)",
                         mag_cfg->declination_deg, mag_cfg->offset_x, mag_cfg->offset_y);
            } else {
                ESP_LOGW(TAG, "HMC5883L init failed: %s (mag yaw will drift)", esp_err_to_name(mag_ret));
                i2c_master_bus_rm_device(s_mag_dev);
                s_mag_dev = NULL;
            }
        } else {
            ESP_LOGW(TAG, "HMC5883L I2C add failed: %s", esp_err_to_name(mag_ret));
            s_mag_dev = NULL;
        }
    } else {
        ESP_LOGI(TAG, "Magnetometer disabled in config");
    }

    /* Initialize MPU6050 */
    ret = mpu6050_init(config);
    if (ret != ESP_OK) {
        if (s_mag_dev) {
            i2c_master_bus_rm_device(s_mag_dev);
            s_mag_dev = NULL;
        }
        i2c_master_bus_rm_device(s_i2c_dev);
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        s_i2c_dev = NULL;
        s_mag_available = false;
        return ret;
    }

    /* Create mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "failed to create mutex");
        i2c_master_bus_rm_device(s_i2c_dev);
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        s_i2c_dev = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Initialize latest reading */
    memset(&s_latest_reading, 0, sizeof(s_latest_reading));

    s_initialized = true;
    ESP_LOGI(TAG, "IMU driver initialized");
    return ESP_OK;
}

esp_err_t driver_imu_start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running) {
        ESP_LOGW(TAG, "already running");
        return ESP_OK;
    }

    const imu_config_t *config = sensor_config_get_imu();

    s_running = true;
    s_last_update_us = 0;

    /* Auto-calibrate gyro bias on first start if all biases are zero */
    if (s_gyro_bias_dps[0] == 0.0f && s_gyro_bias_dps[1] == 0.0f && s_gyro_bias_dps[2] == 0.0f) {
        ESP_LOGI(TAG, "Gyro bias not calibrated, running auto-calibration...");
        esp_err_t cal_ret = driver_imu_calibrate_gyro();
        if (cal_ret == ESP_OK) {
            sensor_config_save_imu_bias(s_gyro_bias_dps);
        } else {
            ESP_LOGW(TAG, "Auto-calibration failed, continuing with zero bias");
        }
    } else {
        ESP_LOGI(TAG, "Using saved gyro bias: [%.4f, %.4f, %.4f] dps",
                 s_gyro_bias_dps[0], s_gyro_bias_dps[1], s_gyro_bias_dps[2]);
    }

    BaseType_t xReturned = xTaskCreatePinnedToCore(
        imu_task, "imu_task", 3072, (void *)config, 6, &s_task_handle, 1);

    if (xReturned != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "failed to create IMU task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "IMU started");
    return ESP_OK;
}

esp_err_t driver_imu_stop(void)
{
    if (!s_running) {
        ESP_LOGW(TAG, "not running");
        return ESP_OK;
    }

    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(50)); /* Give task time to exit */
    s_task_handle = NULL;

    ESP_LOGI(TAG, "IMU stopped");
    return ESP_OK;
}

imu_reading_t driver_imu_get_reading(void)
{
    imu_reading_t result;
    memset(&result, 0, sizeof(result));

    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        result = s_latest_reading;
        xSemaphoreGive(s_mutex);
    }

    return result;
}

void driver_imu_set_yaw(float deg)
{
    while (deg >= 360.0f) deg -= 360.0f;
    while (deg <    0.0f) deg += 360.0f;

    /* Update integration base so future gyro cycles start from the aligned value */
    s_yaw = deg;

    /* Immediately publish so consumers don't see a stale value before next imu_update */
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_latest_reading.yaw_deg = deg;
        xSemaphoreGive(s_mutex);
    }

    ESP_LOGI(TAG, "yaw forced to %.1f°", deg);

    /* Also push to nav_situation immediately */
    nav_situation_update_imu(s_latest_reading.roll_deg, s_latest_reading.pitch_deg, deg, s_latest_reading.gyro_dps[2]);
}

esp_err_t driver_imu_calibrate_gyro(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    const imu_config_t *config = sensor_config_get_imu();

    ESP_LOGI(TAG, "Calibrating gyro - keep IMU stationary for 5 seconds...");

    float bias_sum[3] = {0.0f, 0.0f, 0.0f};
    const int samples = 500;

    for (int i = 0; i < samples; i++) {
        int16_t accel[3], gyro[3];
        esp_err_t ret = mpu6050_read_raw(config, accel, gyro);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "read failed during calibration: %s", esp_err_to_name(ret));
            return ret;
        }

        bias_sum[0] += (float)gyro[0] / GYRO_SCALE_250DPS;
        bias_sum[1] += (float)gyro[1] / GYRO_SCALE_250DPS;
        bias_sum[2] += (float)gyro[2] / GYRO_SCALE_250DPS;

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_gyro_bias_dps[0] = bias_sum[0] / (float)samples;
    s_gyro_bias_dps[1] = bias_sum[1] / (float)samples;
    s_gyro_bias_dps[2] = bias_sum[2] / (float)samples;

    ESP_LOGI(TAG, "Gyro calibration complete: bias=[%.3f, %.3f, %.3f] dps",
             s_gyro_bias_dps[0], s_gyro_bias_dps[1], s_gyro_bias_dps[2]);

    return ESP_OK;
}

mag_status_t driver_imu_get_mag_status(void)
{
    mag_status_t st = {0};
    if (!s_mag_available || !s_mag_dev) {
        st.online = false;
        return st;
    }
    st.online = true;
    const magnetometer_config_t *mag_cfg = sensor_config_get_magnetometer();
    if (mag_cfg) {
        st.declination_deg = mag_cfg->declination_deg;
        st.offset_x = mag_cfg->offset_x;
        st.offset_y = mag_cfg->offset_y;
    }

    int16_t mx, my, mz;
    if (hmc5883l_read_raw(&mx, &my, &mz) == ESP_OK) {
        st.raw_x = (float)mx * HMC5883L_GAIN_MG_PER_LSB;
        st.raw_y = (float)my * HMC5883L_GAIN_MG_PER_LSB;
        st.raw_z = (float)mz * HMC5883L_GAIN_MG_PER_LSB;
        if (mag_cfg) {
            st.heading_deg = mag_compute_heading(mx, my, mag_cfg);
        }
    }
    return st;
}

esp_err_t driver_imu_calibrate_mag(void)
{
    if (!s_mag_available || !s_mag_dev) {
        ESP_LOGE(TAG, "Magnetometer not available, cannot calibrate");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Magnetometer calibration: slowly rotate the vehicle 360° (15s)...");

    float min_x = 1e9f, max_x = -1e9f;
    float min_y = 1e9f, max_y = -1e9f;
    const int samples = 300; /* 15s @ 50ms */

    for (int i = 0; i < samples; i++) {
        int16_t mx, my, mz;
        esp_err_t ret = hmc5883l_read_raw(&mx, &my, &mz);
        if (ret == ESP_OK) {
            float fx = (float)mx * HMC5883L_GAIN_MG_PER_LSB;
            float fy = (float)my * HMC5883L_GAIN_MG_PER_LSB;
            if (fx < min_x) min_x = fx;
            if (fx > max_x) max_x = fx;
            if (fy < min_y) min_y = fy;
            if (fy > max_y) max_y = fy;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    float offset_x = (min_x + max_x) / 2.0f;
    float offset_y = (min_y + max_y) / 2.0f;

    ESP_LOGI(TAG, "Mag calibration complete: min/max X=(%.1f,%.1f) Y=(%.1f,%.1f) offset=(%.1f,%.1f)",
             min_x, max_x, min_y, max_y, offset_x, offset_y);

    /* Persist to sensors.json */
    esp_err_t ret = sensor_config_save_mag_offset(offset_x, offset_y);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save mag offset: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
}
