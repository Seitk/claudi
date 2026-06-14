// claudi_board impl for the ESPWatch 1.69 (240x280 ST7789V2, SPI).
//
// STUB (Phase 1): the real panel / touch / ADC bring-up is Phase 2, once the
// device's actual pinout and flash size are confirmed on hardware. This compiles
// and links so the multi-board build graph is valid and `make watch` produces a
// (non-displaying) image. claudi_board_display_start() returns NULL, so main
// aborts cleanly rather than faking a display.
#include "claudi_board.h"
#include "esp_log.h"

static const char *TAG = "board.watch169";

lv_display_t *claudi_board_display_start(void)
{
    ESP_LOGE(TAG, "watch169 display bring-up not implemented yet (Phase 2)");
    return NULL;
}

esp_err_t claudi_board_backlight_on(void) { return ESP_ERR_NOT_SUPPORTED; }

bool claudi_board_lock(int timeout_ms) { (void)timeout_ms; return true; }
void claudi_board_unlock(void) {}

i2c_master_bus_handle_t claudi_board_i2c_handle(void) { return NULL; }

int  claudi_board_battery_percent(void) { return -1; }
bool claudi_board_charging(void)        { return false; }

const claudi_board_info_t *claudi_board_info(void)
{
    static const claudi_board_info_t info = {
        .name = "watch169", .width = 240, .height = 280,
        .shape = CLAUDI_SHAPE_RECT, .has_pmu = false, .has_rtc = true,
    };
    return &info;
}
