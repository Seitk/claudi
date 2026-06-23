/*
 * claudi_imu — minimal QMI8658 accelerometer driver over the shared board I2C
 * bus (ESP-IDF new i2c_master driver). We only need the accelerometer (for a
 * tilt game), so the gyro stays off to save power.
 *
 * Register map (QST QMI8658C datasheet):
 *   WHO_AM_I 0x00 -> 0x05      CTRL1 0x02 (serial cfg, ADDR auto-increment)
 *   CTRL2 0x03 (accel FS|ODR)  CTRL7 0x08 (sensor enable)
 *   AX_L 0x35 .. AZ_H 0x3A     (6 bytes, little-endian signed 16-bit)
 *
 * Configured for +/-2g full scale => 16384 LSB/g.
 */
#include "claudi_imu.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "claudi_imu"

// The QMI8658 answers at 0x6B (SA0 high) on the Waveshare boards; 0x6A is the
// fallback (SA0 low). We probe both.
#define QMI8658_ADDR_PRIMARY   0x6B
#define QMI8658_ADDR_SECONDARY 0x6A

#define QMI8658_REG_WHOAMI 0x00
#define QMI8658_REG_CTRL1  0x02
#define QMI8658_REG_CTRL2  0x03
#define QMI8658_REG_CTRL7  0x08
#define QMI8658_REG_AX_L   0x35

#define QMI8658_WHOAMI_VAL 0x05

// CTRL1: bit6 ADDR_AI (auto-increment on burst reads) — required to read the 6
// accel bytes in one transfer; SIM/BE left 0 (little-endian, I2C).
#define QMI8658_CTRL1_INIT 0x40
// CTRL2: accel full-scale +/-2g (FS field 0) | ODR ~250Hz (0x05).
#define QMI8658_CTRL2_INIT 0x05
// CTRL7: enable accelerometer only (aEN=bit0), gyro disabled.
#define QMI8658_CTRL7_INIT 0x01

// +/-2g => 32768/2 counts per g.
#define QMI8658_LSB_PER_G 16384.0f

#define I2C_TIMEOUT_MS 100

static i2c_master_dev_handle_t s_dev = NULL;
static bool s_available = false;

static esp_err_t imu_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t imu_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, I2C_TIMEOUT_MS);
}

// Add a device at `addr` and check WHO_AM_I. Returns true on a match; on a
// mismatch it removes the device so the caller can try the other address.
static bool try_addr(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return false;
    }

    uint8_t who = 0;
    esp_err_t err = imu_read_regs(QMI8658_REG_WHOAMI, &who, 1);
    if (err == ESP_OK && who == QMI8658_WHOAMI_VAL) {
        ESP_LOGI(TAG, "QMI8658 found at 0x%02X (WHO_AM_I=0x%02X)", addr, who);
        return true;
    }

    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return false;
}

bool claudi_imu_init(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) {
        ESP_LOGW(TAG, "no I2C bus; IMU disabled");
        return false;
    }
    if (s_available) {
        return true;
    }

    if (!try_addr(bus, QMI8658_ADDR_PRIMARY) && !try_addr(bus, QMI8658_ADDR_SECONDARY)) {
        ESP_LOGW(TAG, "QMI8658 not found; tilt features disabled");
        return false;
    }

    esp_err_t err = ESP_OK;
    err |= imu_write_reg(QMI8658_REG_CTRL1, QMI8658_CTRL1_INIT);
    err |= imu_write_reg(QMI8658_REG_CTRL2, QMI8658_CTRL2_INIT);
    err |= imu_write_reg(QMI8658_REG_CTRL7, QMI8658_CTRL7_INIT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 config write failed");
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return false;
    }

    // Let the accel spin up before the first read.
    vTaskDelay(pdMS_TO_TICKS(20));
    s_available = true;
    ESP_LOGI(TAG, "QMI8658 ready (accel +/-2g)");
    return true;
}

bool claudi_imu_available(void)
{
    return s_available;
}

bool claudi_imu_read_accel(float *ax, float *ay, float *az)
{
    if (!s_available || s_dev == NULL) {
        return false;
    }

    uint8_t raw[6];
    if (imu_read_regs(QMI8658_REG_AX_L, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }

    int16_t rx = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    int16_t ry = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    int16_t rz = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));

    if (ax) *ax = (float)rx / QMI8658_LSB_PER_G;
    if (ay) *ay = (float)ry / QMI8658_LSB_PER_G;
    if (az) *az = (float)rz / QMI8658_LSB_PER_G;
    return true;
}
