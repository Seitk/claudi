# claudi multi-board HAL — Phase 1 (board-independent) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restructure the existing AMOLED firmware behind a `claudi_board` HAL with fully responsive (screen-size-driven) layout, and add the build-profile machinery for a second board — without changing how the AMOLED board behaves.

**Architecture:** Introduce a `claudi_board` interface component (header + Kconfig `choice`). The current `main.cpp` display/PMU/lock block moves into a `claudi_board_amoled175` impl that wraps the Waveshare BSP unchanged; `main.cpp` then talks only to the HAL. Layout constants (`233`/`466`) become runtime reads of the live display size, with the pet app branching on board *shape*. A buildable `claudi_board_watch169` *stub* and per-board sdkconfig/partitions prove the multi-board build graph; the real watch bring-up is Phase 2.

**Tech Stack:** ESP-IDF 5.5, ESP-Brookesia (vendored `brookesia_core` 0.6.0-beta2), LVGL 9.4, C/C++17, Waveshare `esp32_s3_touch_amoled_1_75` BSP. Host unit tests via plain `make` (no IDF) for pure logic.

**Source spec:** `docs/superpowers/specs/2026-06-14-espwatch-multiboard-port-design.md`

**Out of scope (→ Phase 2, after the device is read):** real `watch169` ST7789V2/CST816/ADC bring-up, the 240×280 stylesheet, confirmed watch pinout, watch game gating tuning.

---

## File structure

**New:**
- `firmware/components/claudi_board/include/claudi_board.h` — HAL interface + `claudi_board_info_t`/`claudi_shape_t`.
- `firmware/components/claudi_board/Kconfig` — `choice CLAUDI_BOARD` (AMOLED175 default / WATCH169).
- `firmware/components/claudi_board/CMakeLists.txt` — header-only; transitively requires the selected impl.
- `firmware/components/claudi_board_amoled175/claudi_board_amoled175.c` — HAL impl over the Waveshare BSP + `claudi_power`.
- `firmware/components/claudi_board_amoled175/CMakeLists.txt`
- `firmware/components/claudi_board_watch169/claudi_board_watch169.c` — buildable not-yet-implemented stub.
- `firmware/components/claudi_board_watch169/CMakeLists.txt`
- `firmware/components/claudi_ui/include/claudi_ui_layout.h` + `claudi_ui_layout.c` — pure responsive-geometry helper.
- `firmware/components/claudi_ui/test/` — host unit test for the helper (mirrors `claudi_core/test`).
- `firmware/sdkconfig.defaults.amoled175`, `firmware/sdkconfig.defaults.watch169`
- `firmware/partitions_amoled175.csv`, `firmware/partitions_watch169.csv`
- `firmware/Makefile` — `make amoled` / `make watch` wrappers.

**Modify:**
- `firmware/main/main.cpp` — drop `bsp_*`/`ESP_LV_ADAPTER_*`, call the HAL.
- `firmware/main/idf_component.yml` / `firmware/main/CMakeLists.txt` — require `claudi_board` instead of the BSP directly.
- `firmware/components/brookesia_app_claudi/esp_brookesia_app_claudi.cpp` + `.hpp` — responsive layout + shape branch; battery via HAL.
- `firmware/components/brookesia_app_claudi/CMakeLists.txt` — require `claudi_ui`, `claudi_board`.
- The 11 game components' `.cpp` — replace hardcoded center/size with the helper.
- `firmware/sdkconfig.defaults` — remove the board-specific flash/partition lines now carried per profile.

---

## Task 1: Scaffold the `claudi_board` HAL interface

**Files:**
- Create: `firmware/components/claudi_board/include/claudi_board.h`
- Create: `firmware/components/claudi_board/Kconfig`
- Create: `firmware/components/claudi_board/CMakeLists.txt`

- [ ] **Step 1: Write the public header**

Create `firmware/components/claudi_board/include/claudi_board.h`:

```c
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
```

- [ ] **Step 2: Write the Kconfig board choice**

Create `firmware/components/claudi_board/Kconfig`:

```
menu "claudi board"

choice CLAUDI_BOARD
    prompt "Target board"
    default CLAUDI_BOARD_AMOLED175
    help
        Selects which claudi_board implementation is compiled in.

    config CLAUDI_BOARD_AMOLED175
        bool "Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466 round)"

    config CLAUDI_BOARD_WATCH169
        bool "ESPWatch 1.69 (240x280 ST7789V2) [Phase 2 — stub]"
endchoice

endmenu
```

- [ ] **Step 3: Write the CMakeLists (header-only, requires the selected impl)**

Create `firmware/components/claudi_board/CMakeLists.txt`:

```cmake
# Interface-only: the chosen impl component provides the function bodies.
if(CONFIG_CLAUDI_BOARD_WATCH169)
    set(claudi_board_impl claudi_board_watch169)
else()
    set(claudi_board_impl claudi_board_amoled175)
endif()

idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES driver lvgl ${claudi_board_impl}
)
```

- [ ] **Step 4: Verify it parses (no impls yet — expect a missing-component error naming the impl)**

Run: `cd firmware && . ~/esp/esp-idf/export.sh && idf.py reconfigure`
Expected: CMake error referencing `claudi_board_amoled175` not found (proves the header + Kconfig + dependency wiring are syntactically valid). Tasks 2–3 add the impls and this resolves.

- [ ] **Step 5: Commit**

```bash
git add firmware/components/claudi_board
git commit -m "feat(board): add claudi_board HAL interface + Kconfig choice"
```

---

## Task 2: Implement `claudi_board_amoled175` (move the BSP block out of main)

**Files:**
- Create: `firmware/components/claudi_board_amoled175/claudi_board_amoled175.c`
- Create: `firmware/components/claudi_board_amoled175/CMakeLists.txt`
- Reference: `firmware/main/main.cpp:70-102` (the block being relocated)

- [ ] **Step 1: Write the impl, relocating main.cpp's exact display config**

Create `firmware/components/claudi_board_amoled175/claudi_board_amoled175.c`:

```c
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
```

- [ ] **Step 2: Write the CMakeLists**

Create `firmware/components/claudi_board_amoled175/CMakeLists.txt`:

```cmake
# Only build this impl when it is the selected board.
if(NOT CONFIG_CLAUDI_BOARD_AMOLED175)
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS "claudi_board_amoled175.c"
    REQUIRES claudi_board driver lvgl
    PRIV_REQUIRES esp32_s3_touch_amoled_1_75 claudi_power esp_lib_utils
)
```

- [ ] **Step 3: Move the BSP dependency declaration so the impl owns it**

The Waveshare BSP is fetched via `firmware/main/idf_component.yml`. Move that dependency to the impl by creating `firmware/components/claudi_board_amoled175/idf_component.yml`:

```yaml
dependencies:
  waveshare/esp32_s3_touch_amoled_1_75: "2.0.5"
```

Then delete that same `waveshare/esp32_s3_touch_amoled_1_75` entry from `firmware/main/idf_component.yml` (leave the rest of that file intact).

- [ ] **Step 4: Build the AMOLED profile to confirm the impl resolves**

Run: `cd firmware && . ~/esp/esp-idf/export.sh && idf.py reconfigure && idf.py build`
Expected: build succeeds (Task 3 has not yet rewired main, so `main.cpp` still calls `bsp_*` directly — that still links because the BSP is now a transitive dep via `claudi_board` → `claudi_board_amoled175`. If the BSP headers are no longer visible to main, that is expected and fixed in Task 3.)
Note: if main fails to find `bsp/...` headers here, proceed to Task 3 which removes those includes — the two tasks land together. Commit only after Task 3 builds clean.

- [ ] **Step 5: Commit**

```bash
git add firmware/components/claudi_board_amoled175 firmware/main/idf_component.yml
git commit -m "feat(board): amoled175 HAL impl wrapping the Waveshare BSP"
```

---

## Task 3: Rewire `main.cpp` onto the HAL

**Files:**
- Modify: `firmware/main/main.cpp`
- Modify: `firmware/main/CMakeLists.txt`

- [ ] **Step 1: Replace the BSP includes and display/PMU/lock block**

In `firmware/main/main.cpp`, replace the BSP includes (lines ~10-11):

```cpp
#include "bsp/esp-bsp.h"
#include "bsp/esp32_s3_touch_amoled_1_75.h"
```

with:

```cpp
#include "claudi_board.h"
```

Then replace the whole display bring-up + PMU + lock block (current lines ~65-102, from the `bsp_display_cfg_t disp_cfg = {` comment through the `LvLock::registerCallbacks(...)` call) with:

```cpp
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
```

Also remove the now-unused `#include "claudi_power.h"` line and its `extern "C"` neighbor if power is no longer referenced in main (it is now owned by the board impl). Keep `claudi_net.h` and `claudi_imu.h`.

- [ ] **Step 2: Update main's component requirements**

In `firmware/main/CMakeLists.txt`, in the `idf_component_register(... REQUIRES ...)` list, remove `esp32_s3_touch_amoled_1_75` and `claudi_power` if present, and add `claudi_board`. Keep `claudi_net`, `claudi_imu`, `brookesia_core`, the brookesia app components, etc.

- [ ] **Step 3: Build the AMOLED profile**

Run: `cd firmware && . ~/esp/esp-idf/export.sh && idf.py build`
Expected: PASS, clean link. No `bsp_*` symbols referenced from `main.o` (the HAL owns them now).

- [ ] **Step 4: On-device smoke test (manual — requires the AMOLED board)**

Run: `idf.py -p $(ls /dev/cu.usbmodem*) flash monitor` then `firmware/tools/replay_snapshot.sh http://claudi.local`
Expected: pet, status arc, bubbles, approval card, and all 11 games behave exactly as before the refactor. This is the behavior-preservation gate for the whole HAL extraction.

- [ ] **Step 5: Commit**

```bash
git add firmware/main/main.cpp firmware/main/CMakeLists.txt
git commit -m "refactor(main): drive the display/PMU/lock through claudi_board HAL"
```

---

## Task 4: Pure responsive-geometry helper (`claudi_ui_layout`) — host-tested

**Files:**
- Create: `firmware/components/claudi_ui/include/claudi_ui_layout.h`
- Create: `firmware/components/claudi_ui/claudi_ui_layout.c`
- Create: `firmware/components/claudi_ui/CMakeLists.txt`
- Create: `firmware/components/claudi_ui/test/test_claudi_ui_layout.c`
- Create: `firmware/components/claudi_ui/test/Makefile`

- [ ] **Step 1: Write the failing host test**

Create `firmware/components/claudi_ui/test/test_claudi_ui_layout.c`:

```c
#include <assert.h>
#include <stdio.h>
#include "claudi_ui_layout.h"

int main(void) {
    // 466x466 round: center at (233,233), inscribed radius 233, "safe" inset.
    claudi_layout_t r = claudi_layout_compute(466, 466, CLAUDI_LAYOUT_ROUND);
    assert(r.cx == 233 && r.cy == 233);
    assert(r.short_side == 466);
    assert(r.safe_radius == 233);

    // 240x280 rect: center at (120,140), safe area is the full rect.
    claudi_layout_t w = claudi_layout_compute(240, 280, CLAUDI_LAYOUT_RECT);
    assert(w.cx == 120 && w.cy == 140);
    assert(w.short_side == 240);
    assert(w.safe_radius == 120);   // half the short side

    // scale_q: a 172px asset scaled to ~64% of the short side, in LVGL 0.1% units.
    // 466 short side -> historical 360 (3.6x) for a 172 source ~= 0.385 of side per px? Use the helper's contract:
    // target on-screen size = frac * short_side; scale_q = round(target/src * 256) is NOT used; LVGL uses /256.
    // Helper returns LVGL zoom (256 = 1.0x). Assert monotonicity + known anchor instead of magic numbers:
    uint16_t z466 = claudi_layout_image_zoom(466, 172, 0.66f);
    uint16_t z240 = claudi_layout_image_zoom(240, 172, 0.66f);
    assert(z240 < z466);            // smaller screen -> smaller pet
    assert(z466 == (uint16_t)((466 * 0.66f / 172.0f) * 256.0f));

    printf("all claudi_ui_layout tests passed\n");
    return 0;
}
```

- [ ] **Step 2: Write the header**

Create `firmware/components/claudi_ui/include/claudi_ui_layout.h`:

```c
// claudi_ui_layout.h — pure (hardware-free) responsive geometry so the same app
// code lays out on any resolution/shape. Host-testable; no LVGL/IDF deps.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CLAUDI_LAYOUT_ROUND, CLAUDI_LAYOUT_RECT } claudi_layout_shape_t;

typedef struct {
    int16_t cx, cy;        // screen center
    int16_t short_side;    // min(w,h)
    int16_t safe_radius;   // round: inscribed radius; rect: half short side
} claudi_layout_t;

// Compute the geometry anchors for a given live display.
claudi_layout_t claudi_layout_compute(int16_t w, int16_t h, claudi_layout_shape_t shape);

// LVGL zoom factor (256 == 1.0x) to render a `src_px` asset at `frac` of the
// short side. e.g. zoom for the pet image.
uint16_t claudi_layout_image_zoom(int16_t short_side, int16_t src_px, float frac);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Write the test Makefile and run it (expect FAIL — no impl)**

Create `firmware/components/claudi_ui/test/Makefile` (mirrors `claudi_core/test`):

```make
test:
	cc -std=c11 -I../include -o /tmp/test_claudi_ui_layout \
	    test_claudi_ui_layout.c ../claudi_ui_layout.c && /tmp/test_claudi_ui_layout
```

Run: `cd firmware/components/claudi_ui/test && make test`
Expected: FAIL — `claudi_ui_layout.c` does not exist yet (linker/compile error).

- [ ] **Step 4: Write the minimal implementation**

Create `firmware/components/claudi_ui/claudi_ui_layout.c`:

```c
#include "claudi_ui_layout.h"

claudi_layout_t claudi_layout_compute(int16_t w, int16_t h, claudi_layout_shape_t shape)
{
    claudi_layout_t r;
    r.cx = (int16_t)(w / 2);
    r.cy = (int16_t)(h / 2);
    r.short_side = w < h ? w : h;
    r.safe_radius = (int16_t)(r.short_side / 2);
    (void)shape;  // round and rect share these anchors; shape steers app layout choice
    return r;
}

uint16_t claudi_layout_image_zoom(int16_t short_side, int16_t src_px, float frac)
{
    if (src_px <= 0) return 256;
    return (uint16_t)(((float)short_side * frac / (float)src_px) * 256.0f);
}
```

- [ ] **Step 5: Run the test (expect PASS)**

Run: `cd firmware/components/claudi_ui/test && make test`
Expected: `all claudi_ui_layout tests passed`

- [ ] **Step 6: Write the component CMakeLists**

Create `firmware/components/claudi_ui/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "claudi_ui_layout.c"
    INCLUDE_DIRS "include"
)
```

- [ ] **Step 7: Commit**

```bash
git add firmware/components/claudi_ui
git commit -m "feat(ui): pure responsive-geometry helper with host tests"
```

---

## Task 5: Make the claudi pet app responsive (shape-branched)

**Files:**
- Modify: `firmware/components/brookesia_app_claudi/esp_brookesia_app_claudi.cpp`
- Modify: `firmware/components/brookesia_app_claudi/esp_brookesia_app_claudi.hpp`
- Modify: `firmware/components/brookesia_app_claudi/CMakeLists.txt`

- [ ] **Step 1: Add the deps**

In `firmware/components/brookesia_app_claudi/CMakeLists.txt`, add `claudi_ui` and `claudi_board` to `REQUIRES` (alongside the existing `claudi_core`, `claudi_net`, brookesia, lvgl). Remove a direct `claudi_power` requirement if present (battery now via the HAL).

- [ ] **Step 2: Replace the hardcoded center constants with runtime geometry**

In `esp_brookesia_app_claudi.cpp`, remove `#define SCREEN_CX 233` / `#define SCREEN_CY 233` (and the `ARC_RADIUS`/ring constants that assume 466). At the top of the screen-building function (where `lv_scr_act()` is first used), compute geometry from the live display and board shape:

```cpp
#include "claudi_ui_layout.h"
#include "claudi_board.h"
// ...
const claudi_board_info_t *bi = claudi_board_info();
const bool is_round = (bi->shape == CLAUDI_SHAPE_ROUND);
claudi_layout_t L = claudi_layout_compute(
    lv_display_get_horizontal_resolution(lv_display_get_default()),
    lv_display_get_vertical_resolution(lv_display_get_default()),
    is_round ? CLAUDI_LAYOUT_ROUND : CLAUDI_LAYOUT_RECT);
```

Then replace each literal `233` with `L.cx`/`L.cy`, the ring size `462` with `(L.short_side - 4)`, `ARC_RADIUS (196)` with `(int)(L.safe_radius * 0.84f)`, the bubble offsets `±172` with `±(int)(L.safe_radius * 0.74f)`, the pet scale `360` with `claudi_layout_image_zoom(L.short_side, 172, 1.33f)`, the approval card `400x210` with `(int)(L.short_side*0.86f) x (int)(L.short_side*0.45f)`, and the bottom-status width `300` with `(int)(L.short_side*0.64f)`. (These fractions reproduce today's 466 proportions: 196/233≈0.84, 172/233≈0.74, 360 zoom on a 172 src over 466≈1.33, 400/466≈0.86, 300/466≈0.64.)

- [ ] **Step 3: Branch ring vs. top-strip on shape**

Guard the round-only chrome so the rect board gets a portrait layout:

```cpp
if (is_round) {
    // existing perimeter ring + curved top-arc status word (now using L.*)
} else {
    // RECT/portrait: a top status strip label for the state word at
    // LV_ALIGN_TOP_MID + (0, 6); Wi-Fi/battery/session as small labels in the
    // strip corners; no ring. Pet stays LV_ALIGN_CENTER. Bottom status line
    // at LV_ALIGN_BOTTOM_MID + (0, -6).
}
```

Keep the pet `lv_animimg`, the approval card, and the status line shared (they already use `LV_ALIGN_CENTER`/`BOTTOM_MID` + the now-dynamic offsets).

- [ ] **Step 4: Route the battery read through the HAL**

Replace any `claudi_power_battery_percent()` / `claudi_power_charging()` calls in this file with `claudi_board_battery_percent()` / `claudi_board_charging()`. Hide the battery bubble when the result is `-1`.

- [ ] **Step 5: Build the AMOLED profile and confirm round layout is preserved**

Run: `cd firmware && . ~/esp/esp-idf/export.sh && idf.py build`
Expected: PASS. On-device (AMOLED): the round layout is visually identical to before (the fractions reproduce the old pixel values within ±1 px).

- [ ] **Step 6: Commit**

```bash
git add firmware/components/brookesia_app_claudi
git commit -m "feat(ui): responsive, shape-branched claudi pet layout"
```

---

## Task 6: Make the 11 games responsive

**Files (modify the `.cpp` in each):**
- `brookesia_app_tappop`, `brookesia_app_bullseye`, `brookesia_app_colormatch`,
  `brookesia_app_doodle`, `brookesia_app_fruitninja`, `brookesia_app_pixel2048`,
  `brookesia_app_pixeljump`, `brookesia_app_pixelreflex`, `brookesia_app_pixelsnake`,
  `brookesia_app_tiltmaze`, `brookesia_app_voiceplane`
- Modify each game's `CMakeLists.txt` to add `claudi_ui` to `REQUIRES`.

- [ ] **Step 1: Apply the helper in every game's setup**

In each game's `run()`/build function, add near the top:

```cpp
#include "claudi_ui_layout.h"
claudi_layout_t L = claudi_layout_compute(
    lv_display_get_horizontal_resolution(lv_display_get_default()),
    lv_display_get_vertical_resolution(lv_display_get_default()),
    CLAUDI_LAYOUT_RECT);  // games use full bounds; round boards over-provide
```

Then replace the per-game magic numbers (from the spec's recon) with `L.*`:
- tappop: `SCREEN_CX/CY 233` → `L.cx/L.cy`; `PLACE_RADIUS 175` → `(int)(L.safe_radius*0.75f)`.
- bullseye: `SCREEN_CX/CY 233` and members `_cx/_cy` → `L.cx/L.cy`.
- colormatch: `SCREEN_CX/CY 233` → `L.cx/L.cy`.
- doodle: `W/H 466` → `L.short_side` (canvas square = `L.short_side`); `SWATCH_ARC_R 197.0f` → `L.safe_radius*0.85f`.
- fruitninja: `SCREEN_CX/CY 233` → `L.cx/L.cy`; `FLOOR_Y/SPAWN_Y` scale by `height/466`.
- pixel2048: `SCREEN_CX/CY 233` → `L.cx/L.cy`; board origin `(466-BOARD_PX)/2` → `L.cx - BOARD_PX/2`.
- pixeljump: `SCREEN_CX/CY 233` → `L.cx/L.cy`; `GROUND_Y 320` → `(int)(height*0.69f)`.
- pixelreflex: `SCREEN_CX/CY 233` → `L.cx/L.cy`; `GRID_ORIGIN` from `L.cx/L.cy`.
- pixelsnake: `SCREEN_HALF 233` → `L.cx` (and use `L.cy` where vertical).
- tiltmaze: `SCREEN_CX/CY 233` → `L.cx/L.cy`; grid origin `(SCREEN_CX-GRID/2)` unchanged form.
- voiceplane: `SCREEN_W/H 466` → live `width/height`; `SCREEN_CX/CY 233` → `L.cx/L.cy`; member `_plane_y 233` → `L.cy`.

- [ ] **Step 2: Build the AMOLED profile**

Run: `cd firmware && . ~/esp/esp-idf/export.sh && idf.py build`
Expected: PASS. On-device (AMOLED, 466): each game renders/plays as before (fractions reproduce old pixels).

- [ ] **Step 3: Commit**

```bash
git add firmware/components/brookesia_app_*
git commit -m "feat(games): drive game geometry from live display size"
```

---

## Task 7: Per-board build profiles + partitions

**Files:**
- Create: `firmware/sdkconfig.defaults.amoled175`, `firmware/sdkconfig.defaults.watch169`
- Create: `firmware/partitions_amoled175.csv`, `firmware/partitions_watch169.csv`
- Modify: `firmware/sdkconfig.defaults` (move board-specific lines out)
- Create: `firmware/Makefile`

- [ ] **Step 1: Split the partition tables**

Copy the current `firmware/partitions.csv` to `firmware/partitions_amoled175.csv` (unchanged: app 6 MB @ 0x9000, assets LittleFS 8 MB @ 0x609000).

Create `firmware/partitions_watch169.csv` sized for 16 MB (assets shrunk; the watch ships 7 games):

```
# name,      type, subtype,  offset,    size
nvs,         data, nvs,      0x9000,    0x6000
phy_init,    data, phy,      0xf000,    0x1000
factory,     app,  factory,  0x10000,   0x600000
assets,      data, spiffs,   0x610000,  0x9F0000
```
(Total ≈ 16 MB. Exact assets size is finalized in Phase 2 once `esptool flash_id` confirms 16 MB.)

- [ ] **Step 2: Write the per-board sdkconfig fragments**

Create `firmware/sdkconfig.defaults.amoled175`:

```
CONFIG_CLAUDI_BOARD_AMOLED175=y
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_amoled175.csv"
```

Create `firmware/sdkconfig.defaults.watch169`:

```
CONFIG_CLAUDI_BOARD_WATCH169=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_watch169.csv"
```

Then in `firmware/sdkconfig.defaults`, remove the now per-board lines (`CONFIG_ESPTOOLPY_FLASHSIZE_32MB`, the `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` line) so they don't override the profile. Leave PSRAM/flash-mode/common lines.

- [ ] **Step 3: Write the Makefile wrappers**

Create `firmware/Makefile`:

```make
IDF_DEFAULTS = sdkconfig.defaults
PORT ?= $(shell ls /dev/cu.usbmodem* 2>/dev/null | head -n1)

amoled:
	idf.py -DSDKCONFIG_DEFAULTS="$(IDF_DEFAULTS);sdkconfig.defaults.amoled175" build
watch:
	idf.py -DSDKCONFIG_DEFAULTS="$(IDF_DEFAULTS);sdkconfig.defaults.watch169" build
flash-amoled:
	idf.py -DSDKCONFIG_DEFAULTS="$(IDF_DEFAULTS);sdkconfig.defaults.amoled175" -p $(PORT) flash monitor
flash-watch:
	idf.py -DSDKCONFIG_DEFAULTS="$(IDF_DEFAULTS);sdkconfig.defaults.watch169" -p $(PORT) flash monitor
.PHONY: amoled watch flash-amoled flash-watch
```

- [ ] **Step 4: Build the AMOLED profile via the wrapper**

Run: `cd firmware && . ~/esp/esp-idf/export.sh && rm -rf build sdkconfig && make amoled`
Expected: PASS, and `build/` reflects 32 MB flash + `partitions_amoled175.csv`.

- [ ] **Step 5: Commit**

```bash
git add firmware/Makefile firmware/sdkconfig.defaults* firmware/partitions_*.csv
git rm firmware/partitions.csv
git commit -m "build: per-board sdkconfig profiles + partition tables + make wrappers"
```

---

## Task 8: Buildable `claudi_board_watch169` stub (proves `make watch`)

**Files:**
- Create: `firmware/components/claudi_board_watch169/claudi_board_watch169.c`
- Create: `firmware/components/claudi_board_watch169/CMakeLists.txt`

- [ ] **Step 1: Write the not-yet-implemented stub**

Create `firmware/components/claudi_board_watch169/claudi_board_watch169.c`:

```c
// claudi_board impl for the ESPWatch 1.69 (240x280 ST7789V2).
// STUB: real panel/touch/ADC bring-up is Phase 2, once the device's actual
// pinout/flash are confirmed. Compiles and links so the build graph is valid.
#include "claudi_board.h"
#include "esp_log.h"

static const char *TAG = "board.watch169";

lv_display_t *claudi_board_display_start(void)
{
    ESP_LOGE(TAG, "watch169 display bring-up not implemented yet (Phase 2)");
    return NULL;   // main aborts cleanly; no fake display.
}
esp_err_t claudi_board_backlight_on(void) { return ESP_ERR_NOT_SUPPORTED; }
bool claudi_board_lock(int t) { (void)t; return true; }
void claudi_board_unlock(void) {}
i2c_master_bus_handle_t claudi_board_i2c_handle(void) { return NULL; }
int  claudi_board_battery_percent(void) { return -1; }
bool claudi_board_charging(void) { return false; }

const claudi_board_info_t *claudi_board_info(void)
{
    static const claudi_board_info_t info = {
        .name = "watch169", .width = 240, .height = 280,
        .shape = CLAUDI_SHAPE_RECT, .has_pmu = false, .has_rtc = true,
    };
    return &info;
}
```

- [ ] **Step 2: Write the CMakeLists**

Create `firmware/components/claudi_board_watch169/CMakeLists.txt`:

```cmake
if(NOT CONFIG_CLAUDI_BOARD_WATCH169)
    idf_component_register()
    return()
endif()

idf_component_register(
    SRCS "claudi_board_watch169.c"
    REQUIRES claudi_board driver lvgl
    PRIV_REQUIRES esp_lib_utils
)
```

- [ ] **Step 3: Build the watch profile**

Run: `cd firmware && . ~/esp/esp-idf/export.sh && rm -rf build sdkconfig && make watch`
Expected: PASS — links the watch stub, 16 MB flash, `partitions_watch169.csv`. (Flashing it would boot but show no display; that's the Phase 2 target.) Then `rm -rf build sdkconfig && make amoled` to confirm the AMOLED still builds after switching back.

- [ ] **Step 4: Commit**

```bash
git add firmware/components/claudi_board_watch169
git commit -m "feat(board): buildable watch169 stub (Phase 2 placeholder impl)"
```

---

## Task 9: Docs + Phase-2 handoff

**Files:**
- Modify: `firmware/README.md` (or `CLAUDE.md` build section)

- [ ] **Step 1: Document the multi-board build**

Add a "Boards" section noting `make amoled` / `make watch`, the HAL (`claudi_board`), and that the watch impl is a Phase-2 stub pending on-device pin confirmation. List the Phase-2 risk checklist from the spec §7.

- [ ] **Step 2: Commit**

```bash
git add firmware/README.md
git commit -m "docs: multi-board build + claudi_board HAL"
```

---

## Self-review notes
- **Spec coverage:** §5.1 HAL → T1; §5.2 amoled impl → T2/T3; §5.4 build/partitions → T7; §5.6 responsive layout → T4/T5/T6; §5.7 reuse → unchanged (claudi_core/net/imu untouched). §5.3 watch impl + §5.5 stylesheet → **Phase 2** (intentionally deferred, device-blocked). §5.6 watch game gating → Phase 2 (needs the watch binary to size).
- **No host test for hardware paths** is intentional: only `claudi_ui_layout` (pure) is unit-tested; HAL/app/game changes are verified by building the AMOLED profile + the existing on-device replay tool, which is the right test for embedded display code.
- **Type consistency:** `claudi_board_*`, `claudi_layout_compute/_image_zoom`, `claudi_layout_t{cx,cy,short_side,safe_radius}`, `claudi_shape_t` used identically across tasks.
