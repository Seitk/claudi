/*
 * claudi-s3 — app entry. Brings up the AMOLED via the Waveshare BSP, starts the
 * ESP-Brookesia Phone system, installs the registered apps (the claudi app
 * self-registers via its plugin macro), then starts Wi-Fi/mDNS/HTTP ingest so
 * the Mac-side Claude Code hook can drive the pet.
 */
#include <new>
#include <vector>

#include "claudi_board.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "Main"
#include "esp_lib_utils.h"

extern "C" {
#include "claudi_net.h"
#include "claudi_imu.h"
}

using namespace esp_brookesia;
using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems::phone;

// TEMP diagnostic: log ESP heap + LVGL pool every 1.5s so we can tell a heap
// leak / LVGL-pool fragmentation apart from a pure render/CPU stall while the
// launcher degrades. Remove once the lag root cause is found.
static void claudi_heap_monitor_task(void *)
{
    for (;;) {
        size_t free_int    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        size_t free_psram  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t min_ever    = esp_get_minimum_free_heap_size();

        lv_mem_monitor_t mon = {};
        {
            LvLockGuard guard;          // LVGL isn't thread-safe; serialize with the GUI task
            lv_mem_monitor(&mon);
        }

        ESP_UTILS_LOGW(
            "HEAPMON int_free=%u int_maxblk=%u psram_free=%u min_ever=%u | "
            "lvgl_used=%u lvgl_free=%u lvgl_frag=%u%% lvgl_maxblk=%u",
            (unsigned)free_int, (unsigned)largest_int, (unsigned)free_psram, (unsigned)min_ever,
            (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.free_size,
            (unsigned)mon.frag_pct, (unsigned)mon.free_biggest_size);

        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}

extern "C" void app_main(void)
{
    ESP_UTILS_LOGI("claudi booting");

    // Board HAL brings up panel + touch + LVGL (and the board's power source).
    // The AMOLED impl applies the 24 KB LVGL task stack the launcher needs.
    if (claudi_board_display_start() == nullptr) {
        ESP_UTILS_LOGE("Display start failed");
        return;
    }
    ESP_UTILS_CHECK_ERROR_EXIT(claudi_board_backlight_on(), "Backlight on failed");

    // QMI8658 accelerometer on the shared I2C bus, for the tilt game. Fails soft.
    if (!claudi_imu_init(claudi_board_i2c_handle())) {
        ESP_UTILS_LOGW("IMU init failed; tilt game falls back to centered");
    }

    // Route LVGL locking through the board mutex (LVGL is not thread-safe).
    LvLock::registerCallbacks([](int timeout_ms) {
        ESP_UTILS_CHECK_FALSE_RETURN(claudi_board_lock(timeout_ms), false,
                                     "Lock failed (timeout_ms: %d)", timeout_ms);
        return true;
    }, []() {
        claudi_board_unlock();
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

    // TEMP diagnostic: heap/LVGL-pool watcher (remove once lag is root-caused).
    xTaskCreate(claudi_heap_monitor_task, "heapmon", 4096, nullptr, 1, nullptr);

    ESP_UTILS_LOGI("claudi ready");
}
