# claudi-s3 — V1 Design: ESP-Brookesia Pet + HUD on the S3-Touch-AMOLED

> Date: 2026-06-13
> Status: Approved design (V1 scope). Implementation plan to follow.
> Author: brainstormed with Claude Code.

## 1. Summary

Port the **claudi** Claude Code companion onto a **Waveshare ESP32-S3-Touch-AMOLED-1.75″**
as an **ESP-Brookesia app** built with **ESP-IDF**. The app consumes the *existing*
Claude Code activity snapshot — emitted unchanged by the current
`.claude/hooks/claudi_hook.py` — and renders a reactive **pet** plus a
**transcript / approval HUD** on the 466×466 AMOLED.

V1 is **display-only**: no touch input, no audio. Those are deliberately deferred
to V2 (touch Approve/Deny) and V3 (voice) and the design leaves clean attach
points for both.

The single hard requirement for V1: **zero host-side changes.** The Mac-side hook
already POSTs the snapshot the device needs; the device must speak the protocol the
hook already emits.

## 2. Target hardware (confirmed live)

Read from the connected board (`esptool flash_id`, USB `303A:1001`, MAC
`44:1b:f6:85:53:f8`):

| Item | Value | Source |
|---|---|---|
| MCU | ESP32-S3 (QFN56, rev v0.2), dual-core + LP core, 240 MHz | chip detect |
| PSRAM | 8 MB embedded (octal) | chip detect |
| Flash | **32 MB detected** (vendor sheet lists 16 MB — trust the chip; partition for 32 MB but verify) | chip detect vs. wiki |
| Radio | Wi-Fi b/g/n + BT 5 LE | chip detect |
| USB | native USB-Serial/JTAG, `/dev/cu.usbmodem3101` | system_profiler |

Board variant: **ESP32-S3-Touch-AMOLED-1.75″**. Peripheral controllers
(from the vendor wiki — to be re-confirmed against the BSP/schematic on first build):

| Peripheral | Controller | V1 use |
|---|---|---|
| AMOLED 466×466 QSPI | **CO5300** | yes (display) |
| Capacitive touch | **CST9217** | no (V2) |
| Audio codec | **ES8311** | no (V3) |
| Echo-cancel / mic ADC | **ES7210** (dual mic) | no (V3) |
| IMU 6-axis | **QMI8658** | no (future: shake→error/`dizzy`) |
| RTC | **PCF85063** | optional (clock when idle) |
| PMU | **AXP2101** | bring-up only (battery later) |
| IO expander | **TCA9554** | as required by BSP |
| SD card | TF via SDMMC | no |
| GPS (optional SKU) | LC76G | no |

> Note: the 1.75″ uses **CO5300 + CST9217**, *not* the SH8601 + FT3168 referenced
> in the repo's old git history (those were the 1.8″ board). The display/touch
> driver selection in the BSP must match the 1.75″.

## 3. What already exists (reused, not rebuilt)

The active claudi firmware today (ESP32-C6 + ST7789, PlatformIO/Arduino) is **not**
ported. What carries over is the *host-side investment and the concepts*:

- **`.claude/hooks/claudi_hook.py`** — already maintains a restart-safe session
  snapshot and `POST`s it to the device. **Unchanged** by V1.
  - Snapshot body: `{total, running, waiting, msg, entries[], prompt{id,tool,hint},
    overlay, overlay_ms, source}` → `POST <base>/snapshot`.
  - Legacy fallback: `GET <base>/pet/state?state=<derived>` (+ `GET /render`).
  - Config via env (`CLAUDI_BASE_URL`, `CLAUDI_TIMEOUT`, …); default base
    `http://claudi.local`.
- **Snapshot → state model** (documented in `Skill/claude-desktop-buddy-analysis.md`
  and mirrored in the hook's `derive_state`). V1 implements the device side of
  this ladder.
- **Pet states** vocabulary: `idle, blink, happy, sleepy, curious, alert, bored,
  working, thinking, attention, idea, excited`.
- **Slime art + `assets/slime-final/` pipeline** (`prep_character.py`-style
  union-bbox → fixed-width → quantize). Re-targeted for 466×466 (see §7).

## 4. Architecture

Five units, each with one purpose, a defined interface, and independent testability.

```
Claude Code event
   │  (stdin JSON: SessionStart, PreToolUse, Notification, …)
   ▼
claudi_hook.py            ── unchanged host hook ──┐
   │  POST /snapshot {counters, entries, prompt, overlay}
   ▼
┌──────────────────────── ESP32-S3 (ESP-IDF) ─────────────────────────┐
│ (3) Ingest:  Wi-Fi STA + mDNS claudi.local + HTTP server            │
│        POST /snapshot · GET /status · GET /pet/state (legacy)        │
│              │ writes SnapshotModel (thread-safe)                    │
│              ▼                                                       │
│ (4) State engine:  snapshot → base state (ladder) + one-shot overlay │
│              │ derives PetState + overlay + staleness                │
│              ▼                                                       │
│ (5) Renderer (LVGL):  pet animation widget + HUD (status/transcript/ │
│              │          approval card)                               │
│ (2) Brookesia app shell:  registers claudi, auto-launch as home      │
│ (1) BSP bring-up:  CO5300 display, LVGL port, PSRAM double-buffer    │
└─────────────────────────────────────────────────────────────────────┘
```

### Unit 1 — BSP bring-up
- Use the Espressif/Waveshare **board support package** for the
  ESP32-S3-Touch-AMOLED-1.75″ (display via `esp_lcd` + CO5300 panel driver over
  QSPI; LVGL port). Touch (CST9217), audio, IMU, RTC, PMU drivers are initialized
  by the BSP but **unused in V1** beyond what bring-up requires.
- LVGL configured with a **PSRAM double-buffer** (466×466×2 ≈ 435 KB/buffer — fits
  8 MB PSRAM comfortably; double-buffer for tear-free animation).
- **Interface:** `bsp_init()` → ready LVGL display; `lv_disp_t*`.
- **Depends on:** board only.

### Unit 2 — Brookesia app shell
- Initialize the ESP-Brookesia system and register **claudi** as an app.
- **Auto-launch claudi as the home/primary app on boot** so the device's default
  face is the companion (the launcher remains reachable; claudi is "home").
- App lifecycle hooks (`run`/`back`/`close`/`pause`/`resume` per the pinned
  Brookesia version) — V1 keeps state updating even when not foreground so it's
  current on resume.
- **Pin a specific ESP-Brookesia version** (it churns; v0.4.x-era at design time)
  in `idf_component.yml`.
- **Interface:** an `ESP-Brookesia app` object that owns one full-screen LVGL root.
- **Depends on:** Unit 1.

### Unit 3 — Ingest (network I/O)
- Wi-Fi **STA** joins the home network (reuse existing creds; keep a SoftAP
  fallback for first-boot config as the C6 firmware did). **mDNS** advertises
  `claudi.local`.
- HTTP server endpoints (match what the hook already calls):
  - `POST /snapshot` — body = the hook's snapshot JSON; parse → `SnapshotModel`;
    respond `200 {"ok":true,...}`.
  - `GET /status` — `{name, state (effective/base/overlay), snapshot{}, sys{up,
    heap,psram,fsFree}, net}` for `--ping`/debugging.
  - `GET /pet/state?state=<name>` and `GET /render?...` — legacy fallback so the
    hook's secondary path also works.
- Pure I/O; no rendering. Writes a thread-safe `SnapshotModel` consumed by Unit 4.
- **Interface:** `SnapshotModel` (mutex-guarded struct) + "updated" timestamp.
- **Depends on:** nothing else.

### Unit 4 — State engine
- Pure logic over `SnapshotModel`. Priority ladder (mirror of the hook's
  `derive_state` and the analysis doc):
  ```
  waiting>0 OR prompt set   → attention   (highest)
  running>0                 → working
  otherwise                 → idle
  no snapshot for 30 s      → sleepy       (device-side staleness)
  ```
- **One-shot overlay:** if the snapshot carries `overlay` (+`overlay_ms`), show it
  for that duration, then auto-revert to the derived base. Overlay never sticks.
- Validates overlay against the known state set; unknown → ignored.
- **Interface:** `PetState derive(const SnapshotModel&, now)` → `{base, overlay,
  effective, stale}`; no LVGL calls.
- **Depends on:** Unit 3's data.

### Unit 5 — Renderer (LVGL)
- **Pet widget:** plays a per-state animation. Asset transport:
  **`lv_gif`** decoding GIFs from a **LittleFS partition** (OTA-swappable, ~10–50×
  smaller than raw RGB565 arrays; the 32 MB flash + 8 MB PSRAM make this easy).
  Each pet state maps to one GIF (idle may be an array → carousel).
- **HUD** (see §5) drawn as LVGL labels/objects layered over the pet: status chip,
  transcript list, approval card (read-only in V1), connection indicator.
- Driven by an LVGL timer that reads Unit 4's derived state each tick and only
  swaps animation/labels on change (avoid per-frame churn).
- **Interface:** `render_apply(const Derived&)`.
- **Depends on:** Unit 4.

## 5. Screen layout (466×466 square, V1 display-only)

```
┌─────────────────────────────────┐  466×466 AMOLED (square)
│ ● working        3/12     wifi ✓ │  status chip: state · running/total · link
│                                 │
│                                 │
│           ( pet GIF )           │  centered, full-bleed, PSRAM double-buffered
│                                 │
│   ┌─────────────────────────┐   │
│   │ ! approve  Bash    (4s) │   │  approval card — visible only when waiting;
│   │   rm -rf /tmp/foo       │   │  V1 read-only, V2 adds tap Approve/Deny
│   └─────────────────────────┘   │
│  you: build it                  │  transcript HUD: last ~5 entries,
│  > Read   ok Read   > Edit      │  newest bright → older dim
└─────────────────────────────────┘
```

- **Square layout** (not the portrait 1.8″ mock): pet centered; status chip pinned
  top; transcript pinned bottom; approval card floats above transcript only when
  `waiting`/`prompt`.
- **Approval emphasis** (display-only in V1, per analysis doc P0 minus the input):
  on `attention`, force the screen awake, keep it lit, and show the card with the
  tool + hint + elapsed seconds; after ~10 s the card escalates (color shift) to
  signal a stale pending approval. (Actually *approving* is V2.)

## 6. Data flow

```
Claude Code event → claudi_hook.py → POST /snapshot
  → Ingest parses → SnapshotModel (mutexed, timestamped)
  → LVGL timer tick → State engine derive(model, now)
  → Renderer applies {pet animation, status chip, transcript, approval card}
```

Staleness: if `now − model.updated > 30 s`, engine yields `sleepy` and the
connection indicator flips to "stale".

## 7. Asset pipeline

- Re-render the existing slime art for **466×466**: reuse the
  `assets/slime-final/` union-bbox → fixed-width → flatten-bg → quantize approach,
  changing `TARGET_W`/canvas to the square geometry and emitting **multi-frame
  GIFs per state** (`loop=0, disposal=1`), optionally `gifsicle --lossy`.
- Output to a **LittleFS image** flashed to a dedicated partition (`idf.py`
  `littlefs` / `partition` flow), so art can be replaced without rebuilding
  firmware (OTA-friendly).
- Manifest: `characters/<name>/manifest.json` (`name`, `colors`, `states`; a state
  value may be a filename or an array → carousel).

## 8. Testing & verification (no Claude Code required)

- **Host logic:** existing `python3 .claude/hooks/claudi_hook.py --self-test`
  already validates the snapshot/ladder/overlay logic end-to-end (dry-run).
- **Device protocol:** a `curl` replay script POSTs canned snapshots
  (`{running:1}`, `{waiting:1,prompt:{…}}`, stale-gap, each overlay) to
  `http://claudi.local/snapshot` and asserts `GET /status.effective` matches the
  expected pet state — drives every state without Claude.
- **Connectivity:** `python3 .claude/hooks/claudi_hook.py --ping` lights the pet.
- **Build:** `idf.py build` (optionally in CI); `idf.py flash monitor` to observe
  serial logs that print each derived state transition.
- **Visual:** manual on-device check of each state's animation + HUD against §5.

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| ESP-Brookesia API churn | Pin an exact version in `idf_component.yml`; isolate Brookesia calls in Unit 2 so an upgrade touches one file. |
| Wrong display/touch driver for 1.75″ (CO5300/CST9217 vs old SH8601/FT3168) | Use the board's own BSP; verify panel init on first flash before building UI. |
| Our Wi-Fi/HTTP server coexisting with Brookesia | Both are ESP-IDF citizens; run the server on its own task; short timeouts. |
| Flash size mismatch (32 MB chip vs 16 MB sheet) | Confirm with `esptool flash_id` (done: 32 MB); size the partition table to the verified flash. |
| Asset re-render for square aspect | Reuse the existing pipeline; only geometry constants change. |

## 10. Out of scope for V1 (future specs)

- **V2 — Touch Approve/Deny:** CST9217 touch + a *blocking* return-channel hook
  (today's hook is fire-and-forget; on-device approval needs a hook that waits for
  the device's decision). New endpoint + decision protocol.
- **V3 — Voice:** ES8311/ES7210 mic+speaker, wake word or push-to-talk, STT/TTS,
  routing to Claude. Its own multi-stage project.
- **Later:** IMU shake → `dizzy`/error state; RTC clock face when idle; AXP2101
  battery UI; SD-card asset packs; BLE as a second transport.

## 11. Acceptance criteria (V1 "done")

1. Board boots into the claudi ESP-Brookesia app as home.
2. `claudi_hook.py` (unchanged) drives the device live: running→`working`,
   approval→`attention`, idle→`idle`, 30 s gap→`sleepy`, overlays flash and revert.
3. HUD shows status chip, last ~5 transcript entries, and the approval card (with
   tool/hint/elapsed) when waiting.
4. The `curl` replay script reproduces every pet state via `GET /status`.
5. Pet art renders correctly at 466×466 from the LittleFS asset partition.
