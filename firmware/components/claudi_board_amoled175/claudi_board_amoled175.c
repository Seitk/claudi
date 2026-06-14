// claudi_board impl for the Waveshare ESP32-S3-Touch-AMOLED-1.75.
// Thin wrapper over the vendor BSP; behavior identical to the pre-HAL main.cpp.
#include "claudi_board.h"
#include "bsp/esp-bsp.h"
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "bsp/display.h"
#include "claudi_power.h"
#include "esp_log.h"

static const char *TAG = "board.amoled175";

lv_display_t *claudi_board_display_start(void)
{
    // The BSP's default LVGL task stack (8 KB) overflows during Brookesia
    // launcher swipes; start with the BSP defaults but a 24 KB stack.
    bsp_display_cfg_t disp_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = { .swap_xy = 0, .mirror_x = 1, .mirror_y = 1 },
    };
    disp_cfg.lv_adapter_cfg.task_stack_size = 24 * 1024;

    lv_display_t *disp = bsp_display_start_with_config(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start_with_config failed");
        return NULL;
    }

    // Battery PMU (AXP2101) on the BSP I2C bus. Fails soft -> battery hidden.
    if (!claudi_power_init(bsp_i2c_get_handle())) {
        ESP_LOGW(TAG, "PMU init failed; battery icon hidden");
    }
    return disp;
}

esp_err_t claudi_board_backlight_on(void) { return bsp_display_backlight_on(); }

bool claudi_board_lock(int timeout_ms) { return bsp_display_lock(timeout_ms); }
void claudi_board_unlock(void)         { bsp_display_unlock(); }

i2c_master_bus_handle_t claudi_board_i2c_handle(void) { return bsp_i2c_get_handle(); }

int  claudi_board_battery_percent(void) { return claudi_power_battery_percent(); }
bool claudi_board_charging(void)        { return claudi_power_charging(); }

const claudi_board_info_t *claudi_board_info(void)
{
    static const claudi_board_info_t info = {
        .name = "amoled175", .width = BSP_LCD_H_RES, .height = BSP_LCD_V_RES,
        .shape = CLAUDI_SHAPE_ROUND, .has_pmu = true, .has_rtc = false,
    };
    return &info;
}
