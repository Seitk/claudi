// claudi_board.h — board hardware-abstraction layer.
// One stable interface; exactly one impl component is compiled in per
// CONFIG_CLAUDI_BOARD_*. Callers (main, apps) never touch bsp_*/esp_lcd directly.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAUDI_SHAPE_ROUND,   // circular panel (e.g. 466x466 AMOLED)
    CLAUDI_SHAPE_RECT,    // rectangular/rounded-rect panel (e.g. 240x280 watch)
} claudi_shape_t;

typedef struct {
    const char    *name;      // short board id, e.g. "amoled175"
    uint16_t       width;     // active horizontal resolution (px)
    uint16_t       height;    // active vertical resolution (px)
    claudi_shape_t shape;
    bool           has_pmu;   // true if battery comes from an I2C PMU
    bool           has_rtc;
} claudi_board_info_t;

// Bring up panel + touch + LVGL (and the board's power source). Registers a
// POINTER indev so ESP-Brookesia auto-discovers touch. Returns the LVGL display
// (also set as the default display), or NULL on failure.
lv_display_t *claudi_board_display_start(void);

// Turn the backlight on (call after display_start).
esp_err_t claudi_board_backlight_on(void);

// LVGL lock/unlock (LVGL is not thread-safe). Wired into LvLock by main.
bool claudi_board_lock(int timeout_ms);
void claudi_board_unlock(void);

// Shared I2C master bus (for claudi_imu, and the PMU on boards that have one).
i2c_master_bus_handle_t claudi_board_i2c_handle(void);

// Battery charge 0..100, or -1 if unknown / no battery.
int  claudi_board_battery_percent(void);
bool claudi_board_charging(void);

// Static description of the active board.
const claudi_board_info_t *claudi_board_info(void);

#ifdef __cplusplus
}
#endif
