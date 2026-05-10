#include "drivers/sensor_config.h"
#include "mimi_config.h"
#include "tools/gpio_policy.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "sensor_config";

#define SENSORS_CONFIG_PATH   MIMI_SPIFFS_BASE "/config/sensors.json"

/* Defaults for HC-SR04 (as per specification) */
#define US_LEFT_TRIG_DEF     10
#define US_LEFT_ECHO_DEF     12
#define US_FRONT_TRIG_DEF    13
#define US_FRONT_ECHO_DEF    14
#define US_RIGHT_TRIG_DEF   15
#define US_RIGHT_ECHO_DEF      16
#define US_MAX_RANGE_DEF    400
#define US_ROUND_GAP_DEF    60

/* Defaults for MPU6050 */
#define IMU_I2C_PORT_DEF         0
#define IMU_SDA_GPIO_DEF         8
#define IMU_SCL_GPIO_DEF         9
#define IMU_ADDRESS_DEF          0x68
#define IMU_FREQ_HZ_DEF          400000
#define IMU_SAMPLE_HZ_DEF        100

/* Defaults for NEO-6M GPS */
#define GPS_UART_PORT_DEF        1
#define GPS_RX_GPIO_DEF          17
#define GPS_TX_GPIO_DEF          18
#define GPS_BAUDRATE_DEF         9600

static ultrasonic_config_t s_ultrasonic;
static imu_config_t s_imu;
static gps_config_t s_gps;
static bool s_initialized = false;

static void ultrasonic_load_defaults(void)
{
    s_ultrasonic.left.trig_gpio = US_LEFT_TRIG_DEF;
    s_ultrasonic.left.echo_gpio = US_LEFT_ECHO_DEF;
    s_ultrasonic.front.trig_gpio = US_FRONT_TRIG_DEF;
    s_ultrasonic.front.echo_gpio = US_FRONT_ECHO_DEF;
    s_ultrasonic.right.trig_gpio = US_RIGHT_TRIG_DEF;
    s_ultrasonic.right.echo_gpio = US_RIGHT_ECHO_DEF;
    s_ultrasonic.max_range_cm = US_MAX_RANGE_DEF;
    s_ultrasonic.round_robin_gap_ms = US_ROUND_GAP_DEF;
    s_ultrasonic.loaded = false;
}

static void imu_load_defaults(void)
{
    s_imu.i2c_port = IMU_I2C_PORT_DEF;
    s_imu.sda_gpio = IMU_SDA_GPIO_DEF;
    s_imu.scl_gpio = IMU_SCL_GPIO_DEF;
    s_imu.address = IMU_ADDRESS_DEF;
    s_imu.freq_hz = IMU_FREQ_HZ_DEF;
    s_imu.sample_hz = IMU_SAMPLE_HZ_DEF;
    s_imu.gyro_bias_dps[0] = 0.0f;
    s_imu.gyro_bias_dps[1] = 0.0f;
    s_imu.gyro_bias_dps[2] = 0.0f;
    s_imu.loaded = false;
}

static void gps_load_defaults(void)
{
    s_gps.uart_port = GPS_UART_PORT_DEF;
    s_gps.rx_gpio = GPS_RX_GPIO_DEF;
    s_gps.tx_gpio = GPS_TX_GPIO_DEF;
    s_gps.baudrate = GPS_BAUDRATE_DEF;
    s_gps.loaded = false;
}

esp_err_t sensor_config_load(void)
{
    ultrasonic_load_defaults();
    imu_load_defaults();
    gps_load_defaults();

    FILE *f = fopen(SENSORS_CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No sensors.json found, using defaults");
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4096) {
        fclose(f);
        ESP_LOGW(TAG, "sensors.json size out of range, using defaults");
        return ESP_OK;
    }

    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "malloc failed for sensors.json");
        return ESP_ERR_NO_MEM;
    }
    buf[fread(buf, 1, len, f)] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "sensors.json parse failed, using defaults");
        return ESP_OK;
    }

    cJSON *ultrasonic = cJSON_GetObjectItem(root, "ultrasonic");
    if (ultrasonic) {
        cJSON *v;
        cJSON *left = cJSON_GetObjectItem(ultrasonic, "left");
        if (left) {
            v = cJSON_GetObjectItem(left, "trig_gpio");
            if (cJSON_IsNumber(v)) s_ultrasonic.left.trig_gpio = v->valueint;
            v = cJSON_GetObjectItem(left, "echo_gpio");
            if (cJSON_IsNumber(v)) s_ultrasonic.left.echo_gpio = v->valueint;
        }
        cJSON *front = cJSON_GetObjectItem(ultrasonic, "front");
        if (front) {
            v = cJSON_GetObjectItem(front, "trig_gpio");
            if (cJSON_IsNumber(v)) s_ultrasonic.front.trig_gpio = v->valueint;
            v = cJSON_GetObjectItem(front, "echo_gpio");
            if (cJSON_IsNumber(v)) s_ultrasonic.front.echo_gpio = v->valueint;
        }
        cJSON *right = cJSON_GetObjectItem(ultrasonic, "right");
        if (right) {
            v = cJSON_GetObjectItem(right, "trig_gpio");
            if (cJSON_IsNumber(v)) s_ultrasonic.right.trig_gpio = v->valueint;
            v = cJSON_GetObjectItem(right, "echo_gpio");
            if (cJSON_IsNumber(v)) s_ultrasonic.right.echo_gpio = v->valueint;
        }
        v = cJSON_GetObjectItem(ultrasonic, "max_range_cm");
        if (cJSON_IsNumber(v)) s_ultrasonic.max_range_cm = v->valueint;
        v = cJSON_GetObjectItem(ultrasonic, "round_robin_gap_ms");
        if (cJSON_IsNumber(v)) s_ultrasonic.round_robin_gap_ms = v->valueint;
    }

    cJSON *imu = cJSON_GetObjectItem(root, "imu");
    if (imu) {
        cJSON *v;
        v = cJSON_GetObjectItem(imu, "i2c_port");
        if (cJSON_IsNumber(v)) s_imu.i2c_port = v->valueint;
        v = cJSON_GetObjectItem(imu, "sda_gpio");
        if (cJSON_IsNumber(v)) s_imu.sda_gpio = v->valueint;
        v = cJSON_GetObjectItem(imu, "scl_gpio");
        if (cJSON_IsNumber(v)) s_imu.scl_gpio = v->valueint;
        v = cJSON_GetObjectItem(imu, "address");
        if (cJSON_IsString(v)) s_imu.address = (uint8_t)strtoul(v->valuestring, NULL, 0);
        else if (cJSON_IsNumber(v)) s_imu.address = (uint8_t)v->valueint;
        v = cJSON_GetObjectItem(imu, "freq_hz");
        if (cJSON_IsNumber(v)) s_imu.freq_hz = v->valueint;
        v = cJSON_GetObjectItem(imu, "sample_hz");
        if (cJSON_IsNumber(v)) s_imu.sample_hz = v->valueint;
        cJSON *bias = cJSON_GetObjectItem(imu, "gyro_bias_dps");
        if (cJSON_IsArray(bias) && cJSON_GetArraySize(bias) == 3) {
            s_imu.gyro_bias_dps[0] = (float)cJSON_GetArrayItem(bias, 0)->valuedouble;
            s_imu.gyro_bias_dps[1] = (float)cJSON_GetArrayItem(bias, 1)->valuedouble;
            s_imu.gyro_bias_dps[2] = (float)cJSON_GetArrayItem(bias, 2)->valuedouble;
        }
    }

    cJSON *gps = cJSON_GetObjectItem(root, "gps");
    if (gps) {
        cJSON *v;
        v = cJSON_GetObjectItem(gps, "uart_port");
        if (cJSON_IsNumber(v)) s_gps.uart_port = v->valueint;
        v = cJSON_GetObjectItem(gps, "rx_gpio");
        if (cJSON_IsNumber(v)) s_gps.rx_gpio = v->valueint;
        v = cJSON_GetObjectItem(gps, "tx_gpio");
        if (cJSON_IsNumber(v)) s_gps.tx_gpio = v->valueint;
        v = cJSON_GetObjectItem(gps, "baudrate");
        if (cJSON_IsNumber(v)) s_gps.baudrate = v->valueint;
    }

    cJSON_Delete(root);
    s_ultrasonic.loaded = true;
    s_imu.loaded = true;
    s_gps.loaded = true;
    ESP_LOGI(TAG, "Ultrasonic config loaded: L(GPIO%d/%d F(GPIO%d/%d) R(GPIO%d/%d) max=%dcm gap=%dms",
             s_ultrasonic.left.trig_gpio, s_ultrasonic.left.echo_gpio,
             s_ultrasonic.front.trig_gpio, s_ultrasonic.front.echo_gpio,
             s_ultrasonic.right.trig_gpio, s_ultrasonic.right.echo_gpio,
             s_ultrasonic.max_range_cm, s_ultrasonic.round_robin_gap_ms);
    ESP_LOGI(TAG, "IMU config loaded: I2C%d SDA=GPIO%d SCL=GPIO%d addr=0x%02x freq=%dHz sample=%dHz",
             s_imu.i2c_port, s_imu.sda_gpio, s_imu.scl_gpio, s_imu.address,
             s_imu.freq_hz, s_imu.sample_hz);
    ESP_LOGI(TAG, "GPS config loaded: UART%d RX=GPIO%d TX=GPIO%d baud=%d",
             s_gps.uart_port, s_gps.rx_gpio, s_gps.tx_gpio, s_gps.baudrate);
    return ESP_OK;
}

const ultrasonic_config_t *sensor_config_get_ultrasonic(void)
{
    return &s_ultrasonic;
}

const imu_config_t *sensor_config_get_imu(void)
{
    return &s_imu;
}

const gps_config_t *sensor_config_get_gps(void)
{
    return &s_gps;
}

esp_err_t sensor_config_init(void)
{
    if (!s_initialized) {
        ultrasonic_load_defaults();
        imu_load_defaults();
        gps_load_defaults();
        sensor_config_load();
        s_initialized = true;
        ESP_LOGI(TAG, "sensor config initialized");
    }
    return ESP_OK;
}
