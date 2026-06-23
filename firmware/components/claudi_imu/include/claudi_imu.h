// claudi_imu.h — 3-axis accelerometer reads from the on-board QMI8658 IMU.
//
// The QMI8658 shares the board I2C bus with touch/PMU/codec/RTC; pass the BSP's
// bus handle (bsp_i2c_get_handle()) to claudi_imu_init() — same pattern as
// claudi_power — so we don't re-init the bus. Fails soft: if the chip isn't
// found, claudi_imu_available() returns false and reads return false, so a
// tilt game can fall back gracefully (e.g. keep the ball centered).
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Probe the QMI8658 on `bus` (tries 0x6B then 0x6A), enable the accelerometer
// at +/-2g. Returns false if the chip isn't present (then reads return false).
bool claudi_imu_init(i2c_master_bus_handle_t bus);

// True once a QMI8658 has been found and configured by claudi_imu_init().
bool claudi_imu_available(void);

// Read the accelerometer, in units of g (1.0 == 1 gravity). Returns false if
// the IMU is unavailable or the I2C read failed (outputs left untouched).
// Axes are the raw chip axes; the consumer decides how they map to the screen.
bool claudi_imu_read_accel(float *ax, float *ay, float *az);

#ifdef __cplusplus
}
#endif
