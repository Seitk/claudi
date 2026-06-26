# claudi

**claudi** is a tiny hardware shell around Claude Code: a round-AMOLED desk pet that mirrors agent activity in real time.

It listens to Claude Code hook events, ships session snapshots over HTTP, derives a compact runtime state machine on-device, and renders the result as a living pet + HUD. When the board is on the network, claudi turns "the agent is somewhere in a terminal" into something you can glance at across the room.

Current target hardware: **Waveshare ESP32-S3-Touch-AMOLED-1.75** — 466×466 round AMOLED, CO5300 driver, CST9217 touch, AXP2101 PMU, 8 MB PSRAM, 32 MB flash.

## What lives here

This repo contains the ESP32-S3 firmware, the Claude-side hook plumbing, and the design/spec notes behind the device.

- **Firmware:** `firmware/` — ESP-IDF v5.5 + ESP-Brookesia app for the device UI, networking, and pet behavior
- **Host hook:** `.claude/hooks/` — sends aggregated Claude Code activity snapshots to the device
- **Design / specs:** `docs/superpowers/specs/`
- **Agent guidance:** `CLAUDE.md`

For firmware-specific implementation details, see [`firmware/README.md`](firmware/README.md).

## Runtime path

```text
Claude Code event
  → .claude/hooks/claudi-hook.sh
  → .claude/hooks/claudi_hook.py
  → POST http://claudi.local/snapshot
  → firmware/components/claudi_net
  → firmware/components/claudi_core
  → firmware/components/brookesia_app_claudi
  → AMOLED pet + HUD
```

The host hook is the source of truth for session activity. The device receives snapshot JSON, derives presentation state from it, and paints that state onto the AMOLED as a pet + HUD.

## Repo layout

```text
.
├── CLAUDE.md                          # repo-specific guidance for coding agents
├── README.md                          # top-level project overview
├── docs/superpowers/specs/            # design specs and decision history
├── .claude/hooks/                     # Claude Code hook integration
├── firmware/                          # ESP-IDF firmware project
│   ├── README.md                      # firmware-specific guide
│   ├── components/
│   │   ├── claudi_core/               # portable state model + derivation logic
│   │   ├── claudi_net/                # Wi-Fi, mDNS, HTTP endpoints
│   │   ├── claudi_power/              # battery / PMU integration
│   │   └── brookesia_app_claudi/      # LVGL / ESP-Brookesia UI app
│   ├── main/                          # firmware entry / component wiring
│   └── tools/                         # local helper scripts
└── scripts/                           # misc helper scripts
```

## Bootstrapping

### Prerequisites

- macOS or Linux shell environment
- **ESP-IDF v5.5** installed at `~/esp/esp-idf`
- Board connected over USB when flashing

### Build firmware

```sh
. ~/esp/esp-idf/export.sh
cd firmware
idf.py build
```

### Flash and monitor

```sh
. ~/esp/esp-idf/export.sh
cd firmware
idf.py -p /dev/cu.usbmodem3101 flash monitor
```

Re-check the serial port before flashing:

```sh
ls /dev/cu.usbmodem*
```

### Run host-side tests

Core logic test:

```sh
cd firmware/components/claudi_core/test
make test
```

Hook self-test:

```sh
cd /path/to/claudi
python3 .claude/hooks/claudi_hook.py --self-test
```

Device reachability ping:

```sh
python3 .claude/hooks/claudi_hook.py --ping
```

### Replay device states without Claude Code

```sh
firmware/tools/replay_snapshot.sh http://claudi.local
```

## Architecture notes

- The repo has been pivoted away from the earlier ESP32-C6 + ST7789 PlatformIO/Arduino direction.
- The active implementation target is the **ESP32-S3 Touch AMOLED 1.75** board.
- The snapshot JSON contract between host and device is the key interface; keep both sides in sync when changing it.
- `claudi_core` is intentionally portable and host-tested.

## Development tips

- Prefer `idf.py build` over editor diagnostics; clangd may show false errors in this project.
- Generated pet assets under `brookesia_app_claudi/assets/*.c` should be regenerated, not hand-edited.
- If the device is unreachable at `claudi.local`, host-side tests can still validate most logic.

## Current status

At last verification:

- `firmware/components/claudi_core/test`: PASS
- `.claude/hooks/claudi_hook.py --self-test`: PASS
- `.claude/hooks/claudi_hook.py --ping`: device unreachable at `claudi.local`

## See also

- [`CLAUDE.md`](CLAUDE.md)
- [`firmware/README.md`](firmware/README.md)
- `docs/superpowers/specs/`
