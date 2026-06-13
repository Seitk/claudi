// claudi_power.cpp — AXP2101 battery via XPowersLib over the shared I2C bus.
#include <cstdint>   // must precede XPowersLib.h (its headers use uint8_t early)
#include <cstdio>    // XPowersLib uses printf in its debug paths
#include <cstring>

#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

#include "esp_log.h"

#include "claudi_power.h"

static const char *TAG = "claudi_power";
static XPowersPMU PMU;
static i2c_master_dev_handle_t s_dev = nullptr;
static bool s_ok = false;

// XPowersLib register access callbacks, backed by the i2c_master device handle.
static int pmu_read(uint8_t dev_addr, uint8_t reg, uint8_t *data, uint8_t len)
{
    (void)dev_addr;
    if (s_dev == nullptr) {
        return -1;
    }
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 1000) == ESP_OK ? 0 : -1;
}

static int pmu_write(uint8_t dev_addr, uint8_t reg, uint8_t *data, uint8_t len)
{
    (void)dev_addr;
    if (s_dev == nullptr || len > 31) {
        return -1;
    }
    uint8_t buf[32];
    buf[0] = reg;
    for (uint8_t i = 0; i < len; ++i) {
        buf[i + 1] = data[i];
    }
    return i2c_master_transmit(s_dev, buf, len + 1, 1000) == ESP_OK ? 0 : -1;
}

bool claudi_power_init(i2c_master_bus_handle_t bus)
{
    if (bus == nullptr) {
        return false;
    }
    i2c_device_config_t devcfg = {};
    devcfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    devcfg.device_address = AXP2101_SLAVE_ADDRESS;
    devcfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &devcfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "add AXP2101 device failed");
        return false;
    }
    if (!PMU.begin(AXP2101_SLAVE_ADDRESS, pmu_read, pmu_write)) {
        ESP_LOGE(TAG, "PMU begin failed");
        return false;
    }
    PMU.enableBattVoltageMeasure();
    PMU.enableSystemVoltageMeasure();
    PMU.disableTSPinMeasure();   // no battery-temp sensor on this board
    s_ok = true;
    ESP_LOGI(TAG, "PMU ready, battery=%d%% charging=%d",
             PMU.getBatteryPercent(), (int)PMU.isCharging());
    return true;
}

int claudi_power_battery_percent(void)
{
    return s_ok ? PMU.getBatteryPercent() : -1;
}

bool claudi_power_charging(void)
{
    return s_ok ? PMU.isCharging() : false;
}
