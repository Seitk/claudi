# claudi multi-board port: add the 1.69″ ST7789V2 watch (ESPWatch-S3A3)

- **Date:** 2026-06-14
- **Status:** Approved (design) — ready for implementation planning
- **Supersedes/extends:** the single-board firmware targeting the Waveshare ESP32-S3-Touch-AMOLED-1.75

## 1. Context & problem

claudi today is hard-bound to one board: the Waveshare ESP32-S3-Touch-AMOLED-1.75
(466×466 round CO5300 AMOLED over QSPI, CST9217 touch, AXP2101 PMU, 32 MB flash).
A second device is now in hand — an "ESPWatch-S3A3", which is a
**Waveshare ESP32-S3-Touch-LCD-1.69-class board** (Spotpear sells the identical unit).
The MCU family is the same (ESP32-S3), but the display subsystem is a different
family entirely, so the existing binary cannot render on it.

**Requirement (explicit):** support *both* devices from one codebase — the AMOLED
board stays a first-class target; the watch is added alongside it. This is a
multi-board refactor, not a retarget.

## 2. Target hardware

| | Waveshare 1.75″ AMOLED (existing) | ESPWatch-S3A3 / Waveshare 1.69″ (new) |
|---|---|---|
| MCU / RAM | ESP32-S3R8, 8 MB PSRAM | ESP32-S3R8, 8 MB PSRAM |
| Flash | **32 MB** | **16 MB** |
| Display driver | CO5300 AMOLED | **ST7789V2 IPS** |
| Bus | QSPI | **4-wire SPI** |
| Resolution / shape | 466×466 round | **240×280 rounded-rectangle (portrait)** |
| Touch | CST9217 (I²C) | **CST816T @ 0x15 (I²C)** |
| IMU | QMI8658 @ 0x6B | **QMI8658C @ 0x6B** (same) |
| Power | AXP2101 PMU (I²C) | **No PMU** — ETA6098 charger, battery via ADC on GPIO1, soft power-latch GPIO40/41 |
| RTC | — | PCF85063 @ 0x51 (unused for now) |
| USB | native USB-Serial/JTAG | native USB-Serial/JTAG |

**Provisional watch pinout** (Waveshare docs; *must be confirmed on the physical
unit* — a second published map conflicts): LCD SCLK=GPIO6, MOSI=GPIO7, DC=GPIO4,
CS=GPIO5, RST=GPIO8, BL=GPIO15; shared I²C SDA=GPIO11, SCL=GPIO10; touch RST=GPIO13,
INT=GPIO14; IMU INT1=GPIO38; battery ADC=GPIO1; power-latch SYS_OUT=GPIO40,
SYS_EN=GPIO41; BOOT button=GPIO0; buzzer=GPIO42.

> "ESPWatch-S3A3" is not a hardware SKU — it is a firmware-set USB/BLE/AP name from
> whatever is currently flashed. The hardware identity is the 1.69″ board above.

## 3. Decisions (from brainstorming)

1. **Scope:** full Brookesia port — launcher + games + pet on the watch, not a slim app.
2. **Multi-board, build-time profiles.** One codebase; `CLAUDI_BOARD` selected at
   build time. Runtime auto-detect is rejected: the boards differ in flash size and
   display bus, so they cannot share one flash image.
3. **Watch visual language: rectangular portrait** (top status strip · big centered
   pet · bottom status line). The AMOLED keeps its existing **round** layout.
4. **Watch games: curated subset (~7)** — TapPop, Bullseye, ColorMatch, Pixel Reflex,
   Pixel Jump, Fruit Ninja, Voice Plane. Deferred on the watch: Doodle, Tilt Maze,
   Pixel Snake, 2048 (they need real redesign at 240 px). The AMOLED keeps all 11.
5. **Abstraction seam: a thin `claudi_board` HAL** (not a `bsp_*` compatibility shim,
   not `#ifdef` sprawl). `main.cpp` and apps talk to the HAL only.
6. **Layout becomes dynamic** — the `233`/`466` hardcodes become runtime reads of the
   live display size; the pet app branches on board *shape* (round vs rect).

## 4. Architecture

```
main.cpp (board-agnostic)
   │  claudi_board_display_start() → lv_display_t* · LvLock→claudi_board_lock · Phone begin/install · claudi_net_start
   ▼
claudi_board.h  ── stable HAL interface (new component) ──
   │  display_start · lock/unlock · i2c_handle · backlight_on · battery_percent/charging · board_info{w,h,shape,has_pmu}
   ▼  Kconfig `CLAUDI_BOARD` compiles exactly ONE impl:
   ├─ claudi_board_amoled175  → Waveshare BSP (CO5300 QSPI 466²) + CST9217 + claudi_power(AXP2101) + 32 MB parts
   └─ claudi_board_watch169   → esp_lcd ST7789V2 SPI 240×280 + esp_lvgl_port + CST816 + ADC battery + pwr-latch + 16 MB parts
            │ both hand Brookesia an lv_display_t* + a POINTER indev
            ▼
shared, board-agnostic:  claudi_core · claudi_net · claudi_imu (QMI8658)
                         ESP-Brookesia Phone + stylesheet{466² default | 240×280 new}
                         claudi pet app (responsive: round | portrait) · games (dynamic geometry; ×11 AMOLED / ×7 watch)
```

## 5. Detailed design

### 5.1 `claudi_board` HAL (new component: interface + Kconfig)
```c
typedef enum { CLAUDI_SHAPE_ROUND, CLAUDI_SHAPE_RECT } claudi_shape_t;
typedef struct {
    const char     *name;
    uint16_t        width, height;
    claudi_shape_t  shape;
    bool            has_pmu, has_rtc;
} claudi_board_info_t;

lv_display_t *claudi_board_display_start(void); // panel+touch+lvgl up; returns display; registers POINTER indev
bool          claudi_board_lock(int timeout_ms);
void          claudi_board_unlock(void);
i2c_master_bus_handle_t claudi_board_i2c_handle(void);
esp_err_t     claudi_board_backlight_on(void);
int           claudi_board_battery_percent(void); // 0..100, -1 unknown
bool          claudi_board_charging(void);
const claudi_board_info_t *claudi_board_info(void);
```
The `claudi_board` component carries `choice CLAUDI_BOARD` in Kconfig and a
`REQUIRES` on the selected impl. `main.cpp` loses all `bsp_*` / `ESP_LV_ADAPTER_*` /
`tear_avoid_mode` references; its display block, IMU/PMU bus wiring, and LvLock
callbacks route through the HAL. The claudi pet app's battery read switches from
`claudi_power_*` to `claudi_board_battery_percent/charging`.

### 5.2 `claudi_board_amoled175` (thin wrapper — behavior-preserving)
Relocates today's `main.cpp` display setup unchanged: `bsp_display_start_with_config`
with `ESP_LV_ADAPTER_DEFAULT_CONFIG()`, 24 KB LVGL task stack, `ROTATE_0`,
`TEAR_AVOID_MODE_NONE`, `touch_flags{swap_xy=0,mirror_x=1,mirror_y=1}`, then
`bsp_display_backlight_on`. `claudi_board_i2c_handle` → `bsp_i2c_get_handle`;
lock/unlock → `bsp_display_lock/unlock`; battery → existing `claudi_power` (AXP2101).
`board_info = {"amoled175", 466, 466, ROUND, has_pmu=true, has_rtc=false}`.
**Acceptance: the AMOLED build is functionally identical to today.**

### 5.3 `claudi_board_watch169` (new SPI path)
Depends on `esp_lcd`, `esp_lvgl_port`, `esp_lcd_touch_cst816s`, `esp_adc`.
1. SPI bus init on the LCD pins; `esp_lcd_new_panel_io_spi` → `esp_lcd_new_panel_st7789`
   (`bits_per_pixel=16`, reset pin, RGB element order); `reset → init →`
   `set_gap`/offset (240×280 on a 240×320 controller needs a ~20 px row offset) →
   color-invert as needed → `disp_on_off(true)`.
2. `esp_lvgl_port` init + `lvgl_port_add_disp` → `lv_display_t*` (double buffer in
   PSRAM, `RGB565`, `swap_bytes` as required). `claudi_board_lock/unlock` wrap
   `lvgl_port_lock/unlock`.
3. `esp_lcd_touch_new_i2c_cst816s` on the shared I²C bus → `lvgl_port_add_touch`
   (POINTER indev; Brookesia auto-discovers it). Apply `swap/mirror` after on-device
   touch-orientation check.
4. Backlight: LEDC PWM on the BL pin → `backlight_on`.
5. Battery: ADC oneshot on GPIO1 through the board's divider → percent curve.
   No PMU, so `charging()` is best-effort (USB-Vbus sense if available, else false).
6. Power-latch: assert SYS_EN at boot so the board stays powered off-USB.
7. Shared I²C bus created here and returned by `claudi_board_i2c_handle` for
   `claudi_imu` (and `claudi_power` is *not* linked into this build).
`board_info = {"watch169", 240, 280, RECT, has_pmu=false, has_rtc=true}`.
Pins live in a component header seeded with §2's map, **verified on-device first.**

### 5.4 Build profiles & partitions
- `sdkconfig.defaults` keeps the common settings; add
  `sdkconfig.defaults.amoled175` (32 MB flash, OCT PSRAM, `CLAUDI_BOARD_AMOLED175`,
  `partitions_amoled175.csv`) and `sdkconfig.defaults.watch169` (16 MB flash,
  `CLAUDI_BOARD_WATCH169`, `partitions_watch169.csv`).
- `partitions_amoled175.csv` = today's table (app 6 MB + assets LittleFS 8 MB).
  `partitions_watch169.csv` sized to 16 MB (app 6 MB + a smaller assets partition;
  the watch ships 7 games so assets shrink — exact split set once `flash_id` confirms 16 MB).
- Convenience: `make amoled` / `make watch` wrapping
  `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.<board>" build`.

### 5.5 240×280 Brookesia stylesheet
Brookesia keys stylesheets by exact resolution (`width<<16|height`); `Phone::begin()`
aborts if none matches, but the auto-added default stylesheet is what the 466² board
rides today. Plan: copy `components/brookesia_core/.../stylesheets/480_480/dark` →
`240_280/dark`, add `#include "240_280/dark/stylesheet.hpp"` to the registry, and
`phone->addStylesheet(STYLESHEET_240_280_DARK)` before `begin()` when on the watch.
Retune the absolute-pixel fields (icon container ~120 px, image ~70 px, label font
down a step, status-bar height ~32 px, gesture thresholds ~50→25 px) and add a
240×280 wallpaper asset. App/launcher icon sizing is entirely stylesheet-driven, so
the curated game grid lays itself out with no app-code changes.
**First-boot check:** confirm whether 240×280 needs the custom sheet at all or rides
the calibrated default like 466² — either way the effort is bounded.

### 5.6 Responsive layout (pet + games)
- Replace `SCREEN_CX/CY` (=233) and `W/H` (=466) across the 13 app files with runtime
  reads of `lv_display_get_horizontal/vertical_resolution()` via a small shared helper.
- **claudi pet app — two strategies chosen by `board_info()->shape`:**
  - **ROUND (AMOLED):** existing design preserved (perimeter ring, curved top-arc
    status word, side bubbles, centered pet, bottom status line).
  - **RECT/portrait (watch):** top status strip (state word + Wi-Fi/battery/sessions),
    larger centered pet, bottom status line; approval card + buttons sized to width.
  - Pet art is unchanged; `lv_image_set_scale` is computed from screen size (no regen).
- **Games:** geometry derived from display bounds — round boards clamp interactive
  zones to the inscribed circle, rect uses the full area. The 4 deferred games are
  **compile-gated off the watch build** via the board Kconfig (their WHOLE_ARCHIVE
  registration is excluded), keeping the watch binary smaller; they still ship on the AMOLED.

### 5.7 Reused unchanged
`claudi_core` (state ladder), `claudi_net` (Wi-Fi/mDNS/HTTP + snapshot contract),
`claudi_imu` (QMI8658 @ 0x6B works on both boards), the host hook, and the snapshot
JSON. The `claudi_core` host unit test is unaffected.

## 6. Testing & acceptance
- `claudi_core` host unit test continues to pass (`components/claudi_core/test && make test`).
- **Both profiles build** (`make amoled` and `make watch`) — primary guard that the
  responsive refactor broke neither board.
- AMOLED on-device: visually unchanged from today; all 11 games present.
- Watch on-device: `Phone::begin()` succeeds at 240×280; touch maps correctly;
  pet + status + approval render in portrait; the 7 curated games launch and are
  playable; battery reads a sane percent; the board runs off-USB.
- `firmware/tools/replay_snapshot.sh http://claudi.local` drives every pet/approval
  state on each board.

## 7. Risks / open questions (retire on-device before deep work)
1. **SPI pinmap** — two conflicting published maps; read the silkscreen / shipped
   example sketch / schematic on the actual unit first.
2. **Stylesheet at 240×280** — confirm `begin()` path on first boot (custom vs
   calibrated default).
3. **Power-latch** — confirm what must be driven to stay powered off-USB.
4. **Flash size** — `esptool flash_id` to confirm 16 MB before finalizing partitions.
5. **Touch orientation** — confirm CST816 swap/mirror against the panel.
6. **Display offset/inversion** — confirm the ST7789 240×280 row offset and color inversion.

## 8. Out of scope (for now)
- PCF85063 RTC use on the watch.
- Buzzer/audio feedback on the watch.
- Redesigning Doodle / Tilt Maze / Pixel Snake / 2048 for 240×280.
- Any third board (the HAL is designed so this is cheap later).
