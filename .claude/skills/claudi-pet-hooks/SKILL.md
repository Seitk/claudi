---
name: claudi-pet-hooks
description: Use when working on, debugging, enabling, or extending the Claude Code -> Claudi pet-state hook integration in this repo (the .claude/hooks bridge that drives the ESP32-C6 device's pet states from Claude Code activity).
---

# Claudi Pet Hooks

This repo's firmware (`src/main.cpp`) runs an HTTP server on an ESP32-C6 that
animates a "pet" with a set of named states. This skill documents the bridge
that makes the pet react **live** to Claude Code activity via Claude Code hooks.

## Architecture

```
Claude Code event  ──stdin JSON──▶  .claude/hooks/claudi-hook.sh
   (SessionStart, PreToolUse, ...)        │ (forwards stdin + args)
                                          ▼
                                  .claude/hooks/claudi_hook.py
                          updates host snapshot (claudi_hook_state.json):
                            running / waiting / entries / prompt / msg
                                          ▼
                  HTTP POST http://claudi.local/snapshot  (JSON snapshot)
                  ── fallback if 404/unreachable ──▶ GET /pet/state + /render
                                          ▼
        ESP32-C6 firmware DERIVES the pet state from the snapshot:
          waiting/prompt → attention ; running → working ; else idle ;
          no snapshot 30s → sleepy ; + one-shot overlay (idea/curious/…)
```

Wiring lives in `.claude/settings.json` under `"hooks"`. Each event runs the
wrapper, which calls the Python handler. The handler **never** fails the hook:
all errors are caught and logged, and it always exits 0.

## Files

| File | Role |
|---|---|
| `.claude/settings.json` | Registers the hooks for each Claude Code event. |
| `.claude/hooks/claudi-hook.sh` | Wrapper: finds python3, forwards stdin, always exits 0. |
| `.claude/hooks/claudi_hook.py` | The brain: maintains the host snapshot, POSTs it to the device. |
| `.claude/hooks/claudi_hook_state.json` | Persisted host snapshot (gitignored, auto-created/keyed by session). |
| `.claude/hooks/claudi_hook.log` | Runtime log (created on first run; gitignored-friendly). |

## Snapshot model (how it works now)

The hook is **snapshot-driven**, not event-hard-mapped. Each event mutates a
small persisted snapshot (`apply_event()` in `claudi_hook.py`); the firmware
derives the pet state from it.

**Per-event snapshot mutation** (`apply_event`):

| Claude Code event | Snapshot effect | One-shot overlay |
|---|---|---|
| `SessionStart` | reset all counters | `happy` |
| `UserPromptSubmit` | `waiting=false`, add `you:` entry | `curious` |
| `PreToolUse` | `running++`, `total++`, `waiting=false`, add `>` entry | `curious` (read) / `thinking` (web/task) / — (exec/write) |
| `PostToolUse` | `running--`, add `ok` entry | `idea` (write) / `happy` (bash) / `curious` (web) |
| `Notification` | `waiting=true`, set `prompt{id,tool,hint}` | — |
| `Stop` | `running=0`, clear `waiting`/`prompt` | — |
| `SubagentStop` | `running--` | `happy` |
| `PreCompact` | msg only | — |
| `SessionEnd` | reset counters | `sleepy` |

**Firmware derives the base state** (mirrored client-side in `derive_state()`
for the legacy fallback) via the priority ladder:

```
waiting>0 OR prompt set → attention   (highest)
running>0               → working
otherwise               → idle
no snapshot for 30s      → sleepy       (firmware-side staleness)
```

The optional **overlay** is a one-shot flavour shown ~2.5s then auto-reverted to
the base — so reactions never stick. `running` is reset to 0 on every
`Stop`/`SessionStart`, and the snapshot is keyed by `session_id`, so counters
can't drift forever across crashes/restarts.

Valid device states: `idle, blink, happy, sleepy, curious, alert, bored,
working, thinking, attention, idea, excited` (see `VALID_STATES` and the
firmware's `parsePetState`).

## Configuration (environment variables)

All optional; defaults shown.

| Var | Default | Meaning |
|---|---|---|
| `CLAUDI_BASE_URL` | `http://claudi.local` | Device base URL. Fallbacks: `http://192.168.4.1` (device AP), or its DHCP IP. |
| `CLAUDI_TIMEOUT` | `1.5` | HTTP timeout (seconds). Short so a missing device never stalls Claude. |
| `CLAUDI_ENABLED` | `true` | Master on/off. Set `false` to disable without editing settings. |
| `CLAUDI_DRY_RUN` | `false` | Log/print intended requests, send nothing. |
| `CLAUDI_SEND_MESSAGE` | `true` | Also push a short text line to `/render`. |
| `CLAUDI_ATTEMPTS` | `1` | HTTP attempts per request (retry count). |
| `CLAUDI_LOG_FILE` | `.claude/hooks/claudi_hook.log` | Log path. |
| `CLAUDI_LOG_MAX_BYTES` | `262144` | Truncate log past this size. |
| `CLAUDI_PYTHON` | auto | Override python interpreter used by the wrapper. |

Set these in your shell, or in `.claude/settings.json` under an `"env"` block.

## How to enable

1. Make sure the device is reachable: `curl http://claudi.local/status`
   (or use the AP IP `http://192.168.4.1/status`). If your URL differs, export
   `CLAUDI_BASE_URL`.
2. The hooks in `.claude/settings.json` are picked up automatically by Claude
   Code for this project. Restart the Claude Code session (or run `/hooks`) so
   the new config is loaded.
3. Trigger activity (submit a prompt, run a tool) and watch the pet move.

## Verify / test (no hardware needed)

```sh
# 1. Map every event in dry-run (pure logic check, no network):
python3 .claude/hooks/claudi_hook.py --self-test

# 2. Simulate a single hook payload from stdin in dry-run:
echo '{"hook_event_name":"PreToolUse","tool_name":"Bash"}' \
  | CLAUDI_DRY_RUN=true python3 .claude/hooks/claudi_hook.py

# 3. Byte-compile check:
python3 -m py_compile .claude/hooks/claudi_hook.py

# 4. One-shot helper that runs all of the above:
.claude/hooks/verify.sh

# 5. With hardware on the network, a live connectivity ping:
python3 .claude/hooks/claudi_hook.py --ping
```

## Maintenance notes

- **Adding a new event:** add a branch in `apply_event()` (mutate the snapshot,
  optionally return an overlay) and register the event in `.claude/settings.json`.
- **Changing reactions:** edit `apply_event()` (snapshot effect) and
  `_overlay_for_pre` / `_overlay_for_post` (overlay flavour), then `--self-test`.
- **Changing the base ladder:** edit `deriveBaseState()` in `src/main.cpp` and
  keep `derive_state()` in `claudi_hook.py` (the fallback mirror) in sync.
- **New device states:** if the firmware (`parsePetState` in `src/main.cpp`)
  gains a state, mirror it into `VALID_STATES` here.
- **Inspecting live state:** `curl http://claudi.local/status` returns the full
  derived-state block (`effective_state`, `base_state`, overlay, `snapshot{}`).
- **Debugging:** tail `.claude/hooks/claudi_hook.log`. Every event logs the
  snapshot counters and the HTTP outcome (`[ok]` / `[unreachable]` / `[dry-run]`).
- **Safety invariant:** the handler must always exit 0 and never throw to the
  caller. Preserve the top-level try/except and the wrapper's `|| true`.
