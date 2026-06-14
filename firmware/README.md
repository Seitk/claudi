# claudi firmware

A Claude Code companion device: a reactive pet + transcript/approval HUD, driven
live by Claude Code activity, running as an **ESP-Brookesia** app.

**Multi-board** — one codebase, build-time board selection via the `claudi_board`
HAL. App/UI geometry is responsive (driven by the live display size + board
shape), so the same code lays out on round and rectangular panels.

| Board (`CLAUDI_BOARD`) | Display | Status |
|---|---|---|
| `amoled175` (default) | Waveshare ESP32-S3-Touch-AMOLED-1.75 — 466×466 round CO5300 (QSPI), CST9217 touch, AXP2101 PMU, 32 MB | fully supported |
| `watch169` | ESPWatch 1.69 — 240×280 ST7789V2 (SPI), CST816 touch, QMI8658 IMU, no PMU, 16 MB | **Phase 2** — display bring-up stubbed |

Design specs: `../docs/superpowers/specs/2026-06-13-claudi-s3-amoled-pet-hud-design.md`
and `../docs/superpowers/specs/2026-06-14-espwatch-multiboard-port-design.md`.

## How it works

```
Claude Code event → .claude/hooks/claudi_hook.py → POST http://claudi.local/snapshot
   → claudi_net (Wi-Fi + HTTP) parses JSON into a claudi_snapshot_t
   → claudi_core derives a pet state (ladder + one-shot overlay)
   → brookesia_app_claudi renders the pet + HUD on the AMOLED
```

The Mac-side hook is **unchanged** from the existing project — this firmware
implements the device side of the protocol it already speaks.

## Components

| Component | Role | Hardware/IDF deps |
|---|---|---|
| `claudi_core` | Snapshot model + state-derivation ladder + overlay timing | none (portable C, host-tested) |
| `claudi_net` | Wi-Fi STA + mDNS `claudi.local` + HTTP server (`/snapshot`, `/status`, `/pet/state`) | esp_wifi, esp_http_server, mdns, json |
| `claudi_board` | Board HAL interface (display/lock/i2c/backlight/battery/info) + Kconfig board choice | none (header) |
| `claudi_board_amoled175` | HAL impl wrapping the Waveshare AMOLED BSP + AXP2101 battery | waveshare BSP, claudi_power |
| `claudi_board_watch169` | HAL impl for the 1.69 watch (ST7789V2 SPI) — Phase 2 stub | esp_lcd, esp_lvgl_port (Phase 2) |
| `claudi_ui` | Pure responsive-geometry helper (`claudi_layout_*`), host-tested | none (portable C) |
| `brookesia_app_claudi` | The ESP-Brookesia app: pet + HUD renderer (LVGL), responsive | claudi_board, claudi_ui, brookesia_core, LVGL |
| `brookesia_core` | Vendored ESP-Brookesia 0.6.0-beta2 (Phone system) | LVGL 9.4 |

State ladder (in `claudi_core`): `attention` (waiting/approval) > `working`
(tool running) > `sleepy` (no snapshot 30 s) > `idle`. A one-shot `overlay`
(`idea`/`curious`/`happy`/…) layers on top for `overlay_ms`, then auto-reverts.

## Prerequisites

ESP-IDF **v5.5** with the esp32s3 toolchain installed at `~/esp/esp-idf`:

```sh
. ~/esp/esp-idf/export.sh   # run once per shell
```

## Build / flash / monitor

Pick the board with the make wrappers (each keeps its own `sdkconfig.<board>` and
build dir, so they never cross-contaminate):

```sh
cd firmware
make amoled          # Waveshare 1.75 AMOLED  -> build/
make watch           # ESPWatch 1.69          -> build.watch/
make flash-amoled    # build + flash + monitor on the auto-detected $(PORT)
make flash-watch
```

`export.sh` here needs **Python 3.13**; if it can't find its venv, prepend a shim
(`python3 -> python3.13`) before sourcing:

```sh
export PATH="$HOME/.esp_shim/bin:$PATH"   # ln -s /opt/homebrew/bin/python3.13 ~/.esp_shim/bin/python3
. ~/esp/esp-idf/export.sh
```

Console/logs route to the native USB-Serial/JTAG (the port esptool detects).

## Wi-Fi credentials

Defaults live in `components/claudi_net/claudi_net.c` (`CLAUDI_WIFI_SSID` /
`CLAUDI_WIFI_PASS`, matching the prior board). Override at build time, e.g.:

```sh
idf.py build -DCLAUDI_WIFI_SSID='"MySSID"' -DCLAUDI_WIFI_PASS='"secret"'
```

## Verify without Claude Code

Host unit test of the core logic (no hardware):

```sh
cd components/claudi_core/test && make test
```

Drive the live device through every state (needs the board on the network):

```sh
tools/replay_snapshot.sh http://claudi.local
# or: python3 ../.claude/hooks/claudi_hook.py --ping
```

## On-device approval (V2, opt-in)

The device can approve/deny Claude Code tool calls via a return channel:

- The device shows an **approval card** with on-screen **Approve / Deny / Skip**
  buttons (touch), plus the **BOOT/GPIO0** button (short press = approve, long
  press = deny). "Skip" / timeout defers to the normal terminal prompt.
- `GET /decision?id=<id>` reports the decision; `claudi_net_post_decision()` is
  called by the button/touch handlers.
- The hook (`.claude/hooks/claudi_hook.py`) gates `PreToolUse`: it posts the
  pending tool to the device, polls `/decision`, and returns a
  `permissionDecision` (allow/deny/ask) to Claude Code.

**Enable it** (off by default — when on, gated tools block until you decide,
which also affects the session whose hooks are loaded):

```sh
export CLAUDI_APPROVAL=true            # master switch
export CLAUDI_APPROVAL_TOOLS="Bash,Edit,Write,MultiEdit"  # default "*" = all tools
export CLAUDI_APPROVAL_TIMEOUT=30      # seconds before auto-Skip -> terminal
```

or add these under `"env"` in `.claude/settings.json`. If the device is
unreachable the hook never blocks (normal flow).

> Note: the physical button is wired to GPIO0 (BOOT). This board has two keys +
> an AXP2101 power key; if your button isn't GPIO0, the on-screen buttons still
> work — tell us the GPIO to map the tactile one.

## Roadmap

- **Pet art:** optionally move the embedded images to `lv_gif` packs on the
  `assets` LittleFS partition (OTA-swappable) — currently embedded as LVGL C
  arrays from `tools/gen_pet_assets.py`.
- **V3 — voice:** ES8311/ES7210 mic + speaker (re-add the trimmed AI deps).
