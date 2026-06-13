# claudi-s3 firmware

A Claude Code companion for the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (466×466
CO5300 AMOLED, CST9217 touch, 8 MB PSRAM, 32 MB flash). claudi runs as an
**ESP-Brookesia** app: it shows a reactive pet + transcript/approval HUD driven
live by Claude Code activity.

See the design spec: `../docs/superpowers/specs/2026-06-13-claudi-s3-amoled-pet-hud-design.md`.

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
| `brookesia_app_claudi` | The ESP-Brookesia app: pet + HUD renderer (LVGL) | brookesia_core, LVGL |
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

```sh
cd firmware
idf.py build
idf.py -p /dev/cu.usbmodem3101 flash monitor   # native USB-Serial/JTAG
```

Console/logs are routed to the native USB-Serial/JTAG (the port esptool detects).

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

## V1 scope / roadmap

V1 is **display-only**: the pet is drawn programmatically (a state-colored
animated blob + eyes + HUD). Planned next:

- **Pet art:** swap the programmatic blob for `lv_gif` packs on the `assets`
  LittleFS partition (re-render the existing slime art to 466×466).
- **V2 — touch Approve/Deny:** use CST9217 + a blocking return-channel hook.
- **V3 — voice:** ES8311/ES7210 mic + speaker.
