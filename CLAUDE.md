# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

**claudi** is a Claude Code companion device: a desk pet on a round AMOLED that
reacts live to Claude Code activity (working / waiting-for-approval / idle), shows
status, and can approve/deny tool calls on-device.

Active hardware: **Waveshare ESP32-S3-Touch-AMOLED-1.75** (466×466 round AMOLED,
CO5300 driver, CST9217 touch, AXP2101 PMU, 8 MB PSRAM, 32 MB flash). Serial port
is the native USB-Serial/JTAG, typically `/dev/cu.usbmodem3101` (re-detect with
`ls /dev/cu.usbmodem*` — it can change on replug).

All firmware lives in **`firmware/`** (ESP-IDF v5.5 + ESP-Brookesia). The repo was
pivoted from an earlier ESP32-C6 + ST7789 PlatformIO/Arduino project, now removed;
`firmware/README.md` is the current project readme.

## Build / flash / test

ESP-IDF v5.5 must be installed at `~/esp/esp-idf`. Source it once per shell:

```sh
. ~/esp/esp-idf/export.sh
cd firmware
idf.py build
idf.py -p /dev/cu.usbmodem3101 flash monitor   # re-check the port first
```

Wi-Fi credentials default to constants in `components/claudi_net/claudi_net.c`;
override at build time: `idf.py build -DCLAUDI_WIFI_SSID='"x"' -DCLAUDI_WIFI_PASS='"y"'`.

**Host unit test of the core logic (no hardware, no IDF):**

```sh
cd firmware/components/claudi_core/test && make test
```

**Hook logic test (no hardware):**

```sh
python3 .claude/hooks/claudi_hook.py --self-test     # full event sequence, asserts
python3 .claude/hooks/claudi_hook.py --ping          # light up the device if reachable
echo '{"hook_event_name":"PreToolUse","tool_name":"Bash"}' | CLAUDI_DRY_RUN=true python3 .claude/hooks/claudi_hook.py
```

**Drive every device state without Claude** (board on the network):

```sh
firmware/tools/replay_snapshot.sh http://claudi.local
```

## Architecture: how Claude Code activity reaches the screen

```
Claude Code event → .claude/hooks/claudi-hook.sh → claudi_hook.py
   POST http://claudi.local/snapshot  (aggregated session snapshot, JSON)
        → claudi_net (Wi-Fi + HTTP) parses into a claudi_snapshot_t
        → claudi_core derives a pet state (priority ladder + one-shot overlay)
        → brookesia_app_claudi renders pet + status bubbles + approval card (LVGL)
```

The contract between host and device is the **snapshot JSON** — keep both sides in
sync when changing it. The host hook is the source of truth for activity; the
firmware only derives presentation from the snapshot.

### Host side (`.claude/hooks/`)

`claudi_hook.py` is wired into every Claude Code lifecycle event via
`.claude/settings.json`. Design invariants:
- **Must never break Claude Code**: catches all errors, always exits 0; the `.sh`
  wrapper adds `|| true` and short HTTP timeouts.
- **Multi-session aggregation**: state is a per-session store keyed by `session_id`
  (`load_all`/`aggregate`); `running`/`total` are summed across live sessions,
  `waiting` is true if *any* session waits, and a `sessions` count is sent. Stale
  sessions are TTL-pruned; `SessionEnd` drops a session. Do **not** revert this to
  single-session state.
- **Opt-in on-device approval** (`CLAUDI_APPROVAL=true`, default off): `PreToolUse`
  blocks, posts the pending tool, polls `GET /decision`, and returns a
  `permissionDecision` (allow/deny/ask). It is off by default because enabling it
  makes gated tools block on the device — *including the session whose hooks are
  loaded*. Unreachable device never blocks.
- Tunables are env vars (`CLAUDI_*`); see the CONFIG block at the top of the file.

### Device side (`firmware/components/`)

Three project components + vendored dependencies:

- **`claudi_core`** — portable C: the `claudi_snapshot_t` model and the
  state-derivation ladder (`attention` > `working` > `sleepy`(stale 30s) > `idle`)
  plus the one-shot overlay timing. **No hardware/IDF deps** so it host-compiles
  and is unit-tested. The state ladder is mirrored in the hook's `derive_state`;
  keep them in sync.
- **`claudi_net`** — Wi-Fi STA + mDNS `claudi.local` + HTTP server. Endpoints:
  `POST /snapshot`, `GET /status`, `GET /decision?id=` (approval return channel),
  legacy `GET /pet/state`. Holds the snapshot thread-safely for the UI to poll.
- **`claudi_power`** — AXP2101 battery/charge via XPowersLib over the BSP-shared
  I²C bus (`bsp_i2c_get_handle()`); fails soft (battery icon hidden) if absent.
- **`brookesia_app_claudi`** — the ESP-Brookesia app (LVGL renderer): the pet
  animation, the curved top-arc status word, left/right status bubbles (Wi-Fi,
  battery, session count), and the approval card. Pet art is generated from
  `assets/slime-final/` by `firmware/tools/gen_pet_assets.py` into
  `components/brookesia_app_claudi/assets/*.c` (gitignored — regenerate, don't
  hand-edit).

The stack: claudi app → **ESP-Brookesia** (Phone system, vendored `brookesia_core`)
→ **LVGL 9.4** → **ESP-IDF 5.5 / FreeRTOS** → ESP32-S3. `main/main.cpp` brings up
the BSP display, starts the Brookesia Phone, installs apps from the registry, then
starts `claudi_net`. The board's BSP component (`waveshare/esp32_s3_touch_amoled_1_75`)
is fetched via `main/idf_component.yml` and provides CO5300/CST9217/etc.

### Writing a Brookesia app (the app pattern)

Each app under `firmware/components/brookesia_app_*` (claudi plus several games:
bullseye, colormatch, doodle, fruitninja, tappop, voiceplane) follows the same
shape: subclass `esp_brookesia::systems::phone::App`, build UI on `lv_scr_act()`
in `run()`, and **self-register** with
`ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, <Class>, NAME, ...)`.
Gotchas:
- The register macro token-pastes the class name — call it **inside the namespace
  with the bare class name**, not a qualified one.
- The app component's `CMakeLists.txt` must use `WHOLE_ARCHIVE` or the
  self-registration is stripped and the app never installs.
- Generated LVGL image `.c` files declare **C-linkage** globals; reference them
  from C++ via `extern "C" { LV_IMAGE_DECLARE(...) }`.

## Conventions & gotchas

- **clangd diagnostics are misleading here**: clangd doesn't understand the xtensa
  toolchain flags or the Brookesia/LVGL include graph, so it reports many false
  errors (`-mlongcalls` unknown, `hal.h` not found, plugin-macro template errors).
  Trust `idf.py build`, not the editor squiggles.
- `brookesia_core` is **vendored** at 0.6.0-beta2 with its heavy AI/agent
  dependency stack trimmed from `idf_component.yml` (esp_coze / GMF / websocket —
  unused when `CONFIG_ESP_BROOKESIA_ENABLE_AI_FRAMEWORK=n`, and they conflict with
  IDF 5.5). Re-add them only if enabling the AI framework (e.g. voice).
- The board reports **32 MB** flash even though the vendor sheet says 16 MB — trust
  the chip; `partitions.csv` and `sdkconfig.defaults` are sized for 32 MB.
- Flashing replaces the demo ESP-Brookesia OS image the board ships with; the
  factory firmware lives in the Waveshare sample repo if a restore is needed.
