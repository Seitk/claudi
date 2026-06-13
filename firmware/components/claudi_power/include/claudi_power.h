// claudi_power.h — battery + charge state from the AXP2101 PMU (XPowersLib).
//
// The PMU shares the board I2C bus with touch/codec/RTC; pass the BSP's bus
// handle (bsp_i2c_get_handle()) to claudi_power_init() so we don't re-init it.
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Add the AXP2101 on `bus` and start the PMU. Returns false on failure (then the
// battery reads below return -1 / false and the UI just hides the battery icon).
bool claudi_power_init(i2c_master_bus_handle_t bus);

// Battery charge 0..100, or -1 if unknown / no battery / PMU not initialised.
int claudi_power_battery_percent(void);

// True if the battery is currently charging (USB power in).
bool claudi_power_charging(void);

#ifdef __cplusplus
}
#endif
