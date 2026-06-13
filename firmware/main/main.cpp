/*
 * claudi-s3 — app entry. Brings up the AMOLED via the Waveshare BSP, starts the
 * ESP-Brookesia Phone system, installs the registered apps (the claudi app
 * self-registers via its plugin macro), then starts Wi-Fi/mDNS/HTTP ingest so
 * the Mac-side Claude Code hook can drive the pet.
 */
#include <new>
#include <vector>

#include "bsp/esp-bsp.h"
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "Main"
#include "esp_lib_utils.h"

extern "C" {
#include "claudi_net.h"
#include "claudi_power.h"
}

using namespace esp_brookesia;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

extern "C" void app_main(void)
{
    ESP_UTILS_LOGI("claudi booting");

    ESP_UTILS_CHECK_NULL_EXIT(bsp_display_start(), "Start display failed");
    ESP_UTILS_CHECK_ERROR_EXIT(bsp_display_backlight_on(), "Turn on display backlight failed");

    // Battery PMU (AXP2101) on the BSP I2C bus, for the battery icon.
    if (!claudi_power_init(bsp_i2c_get_handle())) {
        ESP_UTILS_LOGW("PMU init failed; battery icon hidden");
    }

    // Route LVGL locking through the BSP mutex (LVGL is not thread-safe).
    LvLock::registerCallbacks([](int timeout_ms) {
        esp_err_t ret = bsp_display_lock(timeout_ms);
        ESP_UTILS_CHECK_FALSE_RETURN(ret == ESP_OK, false, "Lock failed (timeout_ms: %d)", timeout_ms);
        return true;
    }, []() {
        bsp_display_unlock();
        return true;
    });

    Phone *phone = new (std::nothrow) Phone();
    ESP_UTILS_CHECK_NULL_EXIT(phone, "Create phone failed");

    {
        // Operating on LVGL from this (non-GUI) task: hold the lock.
        LvLockGuard gui_guard;

        ESP_UTILS_CHECK_FALSE_EXIT(phone->begin(), "Begin failed");

        std::vector<systems::base::Manager::RegistryAppInfo> inited_apps;
        ESP_UTILS_CHECK_FALSE_EXIT(phone->initAppFromRegistry(inited_apps), "Init app registry failed");
        ESP_UTILS_CHECK_FALSE_EXIT(phone->installAppFromRegistry(inited_apps), "Install app registry failed");
    }

    // Start networking last so its logs don't interleave with display bring-up.
    // The hook POSTs /snapshot here; the claudi app polls the parsed snapshot.
    claudi_net_start();

    ESP_UTILS_LOGI("claudi ready");
}
