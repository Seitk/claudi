#!/usr/bin/env python3
"""claudi_hook.py — bridge Claude Code activity to the Claudi pet device.

Claude Code invokes this script for various lifecycle events (SessionStart,
UserPromptSubmit, PreToolUse, PostToolUse, Notification, Stop, SubagentStop, ...).
The event payload arrives as JSON on stdin.

Snapshot model (primary path)
-----------------------------
Rather than hard-mapping every single event to one pet state, this hook keeps a
small **host-side session snapshot** in a state file and, after each event,
POSTs the aggregated snapshot to the device:

    POST <base>/snapshot
    {"total":N,"running":N,"waiting":0|1,"msg":"...","entries":[...],
     "prompt":{"id","tool","hint"},"overlay":"idea","overlay_ms":2500,
     "source":"PostToolUse"}

The firmware derives a *base* pet state from the counters via a priority ladder
(waiting/approval > running > idle > sleepy-on-stale) and layers the optional
one-shot `overlay` (idea/curious/happy/excited...) on top for a brief reaction.
This keeps the pet calm and snapshot-true instead of twitching on every tool.

Fallback path
-------------
If `/snapshot` is unreachable (old firmware / 404 / network), the hook falls
back to the legacy `GET /pet/state?state=<derived>` (+ optional `/render`) so it
still works against firmware that predates `/snapshot`.

Design goals
------------
* **Fail safe.** A hook must never block or break Claude Code. Any error is
  logged and we still exit 0. Network problems just mean the pet doesn't move.
* **Stdlib only.** No third-party imports.
* **Restart-safe.** State is keyed by session_id; a new session (or missing /
  corrupt state file) resets the counters so they never drift forever.
* **Configurable via environment variables** (see CONFIG below).
* **Testable without hardware.** `--dry-run` / CLAUDI_DRY_RUN prints what would
  be sent instead of sending it; `--self-test` exercises a full event sequence.
"""

from __future__ import annotations

import contextlib
import json
import os
import sys
import time
import urllib.parse
import urllib.request
import urllib.error

# --------------------------------------------------------------------------- #
# Configuration (all overridable by environment variables)
# --------------------------------------------------------------------------- #

def _env_bool(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in ("1", "true", "yes", "on")


# Base URL of the Claudi device HTTP server. mDNS name by default; common
# fallbacks are http://192.168.4.1 (the device's own AP) or the DHCP lease.
BASE_URL = os.environ.get("CLAUDI_BASE_URL", "http://claudi.local").rstrip("/")

# HTTP timeout in seconds. Kept short so a missing device never stalls Claude.
TIMEOUT = float(os.environ.get("CLAUDI_TIMEOUT", "1.5"))

# Master on/off switch.
ENABLED = _env_bool("CLAUDI_ENABLED", True)

# Dry-run: log + print the intended request, but do not touch the network.
DRY_RUN = _env_bool("CLAUDI_DRY_RUN", False)

# Use the snapshot path (POST /snapshot). When false, only the legacy
# /pet/state path is used.
USE_SNAPSHOT = _env_bool("CLAUDI_SNAPSHOT", True)

# Whether to also push a short text line to the device's /render endpoint when
# using the legacy fallback path.
SEND_MESSAGE = _env_bool("CLAUDI_SEND_MESSAGE", True)

# --- On-device approval (V2) ---------------------------------------------- #
# When enabled, PreToolUse blocks: the pending tool is shown on the device and
# the user approves (short press), denies (long press), or dismisses (screen tap
# / timeout -> defer to the normal terminal prompt). The decision is returned to
# Claude Code as a PreToolUse permissionDecision.
#
# Default OFF: turning this on makes EVERY gated tool wait for the device, which
# also affects the session that has these hooks loaded. Opt in deliberately:
#   export CLAUDI_APPROVAL=true   (or set it in .claude/settings.json "env")
APPROVAL_ENABLED = _env_bool("CLAUDI_APPROVAL", False)
# Seconds to wait for a decision before dismissing (-> normal terminal prompt).
APPROVAL_TIMEOUT = float(os.environ.get("CLAUDI_APPROVAL_TIMEOUT", "30"))
# Poll interval while waiting for the decision.
APPROVAL_POLL = float(os.environ.get("CLAUDI_APPROVAL_POLL", "0.3"))
# Which tools to gate: "*" = all, otherwise a comma-separated tool-name list.
APPROVAL_TOOLS = os.environ.get("CLAUDI_APPROVAL_TOOLS", "*")

# Number of HTTP attempts (1 = no retry). Retries are cheap and bounded.
ATTEMPTS = max(1, int(os.environ.get("CLAUDI_ATTEMPTS", "1")))

# Where to log / persist host-side snapshot state. Default next to this script.
_HERE = os.path.dirname(os.path.abspath(__file__))
LOG_FILE = os.environ.get("CLAUDI_LOG_FILE", os.path.join(_HERE, "claudi_hook.log"))
STATE_FILE = os.environ.get("CLAUDI_STATE_FILE", os.path.join(_HERE, "claudi_hook_state.json"))

# Cap the log file size (bytes); when exceeded we truncate. Keeps things tidy.
LOG_MAX_BYTES = int(os.environ.get("CLAUDI_LOG_MAX_BYTES", str(256 * 1024)))

# Recent transcript entries kept in the snapshot (firmware caps at 5 too).
MAX_ENTRIES = 5

VALID_STATES = {
    "idle", "blink", "happy", "sleepy", "curious", "alert",
    "bored", "working", "thinking", "attention", "idea", "excited",
}

# Tool buckets, reused for the optional one-shot overlay flavour per event.
READ_TOOLS = {"Read", "Glob", "Grep", "LS", "NotebookRead"}
WEB_TOOLS = {"WebFetch", "WebSearch"}
EXEC_TOOLS = {"Bash", "BashOutput", "KillBash"}
WRITE_TOOLS = {"Write", "Edit", "MultiEdit", "NotebookEdit"}
THINK_TOOLS = {"Task", "TodoWrite", "ExitPlanMode"}


# --------------------------------------------------------------------------- #
# Logging
# --------------------------------------------------------------------------- #

def log(msg: str) -> None:
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}\n"
    try:
        try:
            if os.path.exists(LOG_FILE) and os.path.getsize(LOG_FILE) > LOG_MAX_BYTES:
                with open(LOG_FILE, "w", encoding="utf-8") as fh:
                    fh.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} [log truncated]\n")
        except OSError:
            pass
        with open(LOG_FILE, "a", encoding="utf-8") as fh:
            fh.write(line)
    except OSError:
        pass


# --------------------------------------------------------------------------- #
# Host-side snapshot state (persisted, restart-safe)
# --------------------------------------------------------------------------- #

def _fresh_state(session_id: str = "") -> dict:
    """A clean snapshot for a brand-new (or reset) session."""
    return {
        "session_id": session_id,
        "total": 0,        # cumulative tool calls this session
        "running": 0,      # tools currently in flight (Pre - Post)
        "waiting": False,  # waiting on the human (approval / input)
        "entries": [],     # recent transcript lines, oldest first
        "prompt": None,    # {"id","tool","hint"} when an approval is pending
        "msg": "",         # latest one-line status
        "updated": 0.0,
    }


@contextlib.contextmanager
def _state_lock():
    """Best-effort exclusive lock so concurrent hooks don't clobber the file.

    Locking is advisory and entirely optional: if fcntl is unavailable or the
    lock fails, we proceed unlocked. A hook must never block on this.
    """
    fh = None
    try:
        import fcntl  # noqa: PLC0415 - unix only, optional
        fh = open(STATE_FILE + ".lock", "w")
        fcntl.flock(fh, fcntl.LOCK_EX)
    except Exception:  # noqa: BLE001
        if fh is not None:
            try:
                fh.close()
            except Exception:  # noqa: BLE001
                pass
        fh = None
    try:
        yield
    finally:
        if fh is not None:
            try:
                fh.close()  # closing the fd releases the flock
            except Exception:  # noqa: BLE001
                pass


def load_state(session_id: str) -> dict:
    """Load the persisted snapshot, resetting on missing/corrupt/new session."""
    state = None
    try:
        with open(STATE_FILE, "r", encoding="utf-8") as fh:
            state = json.load(fh)
    except (OSError, ValueError):
        state = None

    if not isinstance(state, dict) or "running" not in state:
        return _fresh_state(session_id)

    # A new session id means a fresh process/turn: reset counters so a crash or
    # a missed Post/Stop in a previous session can never drift us forever.
    if session_id and state.get("session_id") and state["session_id"] != session_id:
        log(f"[state] session change {state.get('session_id')!r} -> {session_id!r}, reset")
        return _fresh_state(session_id)

    # Backfill any missing keys defensively.
    base = _fresh_state(session_id or state.get("session_id", ""))
    base.update({k: state.get(k, base[k]) for k in base})
    base["session_id"] = session_id or state.get("session_id", "")
    return base


def save_state(state: dict) -> None:
    state["updated"] = time.time()
    tmp = STATE_FILE + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(state, fh)
        os.replace(tmp, STATE_FILE)
    except OSError as exc:
        log(f"[warn] could not persist state: {exc!r}")


def _add_entry(state: dict, text: str) -> None:
    if not text:
        return
    state["entries"].append(text[:48])
    if len(state["entries"]) > MAX_ENTRIES:
        state["entries"] = state["entries"][-MAX_ENTRIES:]


# --------------------------------------------------------------------------- #
# Event -> snapshot mutation
# --------------------------------------------------------------------------- #

def _overlay_for_pre(tool: str):
    if tool in READ_TOOLS:
        return "curious"
    if tool in WEB_TOOLS or tool in THINK_TOOLS:
        return "thinking"
    return None  # exec/write -> base "working" already looks right


def _overlay_for_post(tool: str):
    if tool in WRITE_TOOLS:
        return "idea"      # just produced/changed code
    if tool in EXEC_TOOLS:
        return "happy"     # a command finished
    if tool in WEB_TOOLS:
        return "curious"   # got results to chew on
    return None


def apply_event(state: dict, payload: dict):
    """Mutate `state` in place for one event; return (overlay, overlay_ms).

    overlay is a transient one-shot flavour (or None); the device auto-reverts
    to the snapshot-derived base after overlay_ms.
    """
    event = payload.get("hook_event_name", "") or payload.get("hook_event", "")
    tool = payload.get("tool_name", "") or ""
    overlay = None
    overlay_ms = 2500

    if event == "SessionStart":
        sid = state.get("session_id", "")
        state.clear()
        state.update(_fresh_state(sid))
        state["msg"] = "session start"
        overlay, overlay_ms = "happy", 2500

    elif event == "UserPromptSubmit":
        state["waiting"] = False
        state["prompt"] = None
        prompt = (payload.get("prompt") or "").strip().replace("\n", " ")
        _add_entry(state, f"you: {prompt}" if prompt else "you: (prompt)")
        state["msg"] = "prompt received"
        overlay, overlay_ms = "curious", 1500

    elif event == "PreToolUse":
        state["waiting"] = False  # Claude is proceeding, not blocked
        state["running"] = max(0, int(state.get("running", 0))) + 1
        state["total"] = int(state.get("total", 0)) + 1
        _add_entry(state, f"> {tool or 'tool'}")
        state["msg"] = f"using {tool or 'tool'}"
        overlay = _overlay_for_pre(tool)

    elif event == "PostToolUse":
        state["running"] = max(0, int(state.get("running", 0)) - 1)
        _add_entry(state, f"ok {tool or 'tool'}")
        state["msg"] = f"done {tool or 'tool'}"
        overlay = _overlay_for_post(tool)

    elif event == "Notification":
        # Claude needs the human: surface as a waiting/approval snapshot so the
        # device derives `attention` (highest priority on the ladder).
        message = (payload.get("message") or "needs you").strip().replace("\n", " ")
        state["waiting"] = True
        state["prompt"] = {"id": "notif", "tool": tool, "hint": message[:60]}
        state["msg"] = message[:48]
        _add_entry(state, f"! {message}")

    elif event == "Stop":
        # Turn boundary: clear in-flight counters so nothing drifts between turns.
        state["running"] = 0
        state["waiting"] = False
        state["prompt"] = None
        state["msg"] = "turn done"

    elif event == "SubagentStop":
        state["running"] = max(0, int(state.get("running", 0)) - 1)
        state["msg"] = "subagent done"
        overlay, overlay_ms = "happy", 2000

    elif event == "PreCompact":
        state["msg"] = "compacting"

    elif event == "SessionEnd":
        state["running"] = 0
        state["waiting"] = False
        state["prompt"] = None
        state["msg"] = "session end"
        overlay, overlay_ms = "sleepy", 4000

    else:
        state["msg"] = event or "event"

    return overlay, overlay_ms


def derive_state(state: dict) -> str:
    """Client-side mirror of the firmware ladder, for the legacy fallback path."""
    if state.get("waiting") or state.get("prompt"):
        return "attention"
    if int(state.get("running", 0)) > 0:
        return "working"
    return "idle"


# --------------------------------------------------------------------------- #
# Device I/O
# --------------------------------------------------------------------------- #

def _http(url: str, data: bytes | None = None, method: str | None = None) -> str:
    last_err: Exception | None = None
    for attempt in range(1, ATTEMPTS + 1):
        try:
            headers = {"User-Agent": "claudi-hook/2.0"}
            if data is not None:
                headers["Content-Type"] = "application/json"
            req = urllib.request.Request(url, data=data, headers=headers, method=method)
            with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
                return resp.read(512).decode("utf-8", "replace")
        except Exception as exc:  # noqa: BLE001 - fail safe, capture everything
            last_err = exc
            if attempt < ATTEMPTS:
                time.sleep(0.1)
    raise last_err if last_err else RuntimeError("unknown http error")


def build_snapshot(state: dict, event: str, overlay, overlay_ms: int) -> dict:
    snap = {
        "total": int(state.get("total", 0)),
        "running": int(state.get("running", 0)),
        "waiting": 1 if state.get("waiting") else 0,
        "msg": state.get("msg", "")[:48],
        "entries": list(state.get("entries", []))[-MAX_ENTRIES:],
        "source": event,
    }
    if state.get("prompt"):
        snap["prompt"] = state["prompt"]
    if overlay:
        snap["overlay"] = overlay
        snap["overlay_ms"] = int(overlay_ms)
    return snap


def send_snapshot(state: dict, event: str, overlay, overlay_ms: int) -> bool:
    snap = build_snapshot(state, event, overlay, overlay_ms)
    derived = derive_state(state)
    effective = overlay if overlay in VALID_STATES else derived
    url = f"{BASE_URL}/snapshot"
    body = json.dumps(snap, separators=(",", ":"))

    if DRY_RUN:
        log(f"[dry-run] POST {url} {body}")
        print(f"[claudi dry-run] event={event} state={effective} "
              f"running={snap['running']} waiting={snap['waiting']} "
              f"overlay={overlay or '-'} -> {url}")
        return True

    if USE_SNAPSHOT:
        try:
            resp = _http(url, data=body.encode("utf-8"), method="POST")
            log(f"[ok] snapshot event={event} running={snap['running']} "
                f"waiting={snap['waiting']} overlay={overlay or '-'} <- {resp.strip()[:120]}")
            return True
        except urllib.error.HTTPError as exc:
            if exc.code != 404:
                log(f"[unreachable] snapshot http {exc.code}; falling back to /pet/state")
            else:
                log("[info] /snapshot 404 (old firmware?); falling back to /pet/state")
        except Exception as exc:  # noqa: BLE001
            log(f"[unreachable] snapshot err={exc!r}; falling back to /pet/state")

    # Legacy fallback: GET /pet/state?state=<effective> (+ optional /render).
    return _send_legacy(effective, snap["msg"])


def _send_legacy(state_name: str, message: str) -> bool:
    if state_name not in VALID_STATES:
        state_name = "idle"
    state_url = f"{BASE_URL}/pet/state?state={urllib.parse.quote(state_name)}"
    ok = True
    try:
        resp = _http(state_url)
        log(f"[ok] legacy state={state_name} <- {resp.strip()[:120]}")
    except Exception as exc:  # noqa: BLE001
        ok = False
        log(f"[unreachable] legacy state={state_name} err={exc!r}")

    if SEND_MESSAGE and message:
        params = urllib.parse.urlencode({"title": "claudi", "line2": "claude-code",
                                         "line3": message[:48]})
        try:
            _http(f"{BASE_URL}/render?{params}")
        except Exception as exc:  # noqa: BLE001
            log(f"[unreachable] render err={exc!r}")
    return ok


# --------------------------------------------------------------------------- #
# Entry points
# --------------------------------------------------------------------------- #

# --------------------------------------------------------------------------- #
# On-device approval flow (PreToolUse return-channel)
# --------------------------------------------------------------------------- #

def _tool_gated(tool: str) -> bool:
    spec = APPROVAL_TOOLS.strip()
    if spec in ("", "*"):
        return True
    return tool in {t.strip() for t in spec.split(",") if t.strip()}


def _approval_hint(payload: dict) -> str:
    ti = payload.get("tool_input") or {}
    if isinstance(ti, dict):
        for key in ("command", "file_path", "path", "url", "pattern", "query", "description"):
            val = ti.get(key)
            if isinstance(val, str) and val.strip():
                return val.strip().replace("\n", " ")[:60]
        try:
            return json.dumps(ti, separators=(",", ":"))[:60]
        except Exception:  # noqa: BLE001
            return ""
    return str(ti)[:60]


def _emit_pretool_decision(decision: str, reason: str) -> None:
    """Print a PreToolUse permissionDecision for Claude Code (allow/deny/ask).

    This is the ONLY thing that may go to stdout; everything else logs to file.
    """
    out = {
        "hookSpecificOutput": {
            "hookEventName": "PreToolUse",
            "permissionDecision": decision,
            "permissionDecisionReason": reason,
        }
    }
    print(json.dumps(out))


def _post_snapshot(snap: dict) -> bool:
    try:
        body = json.dumps(snap, separators=(",", ":")).encode("utf-8")
        _http(f"{BASE_URL}/snapshot", data=body, method="POST")
        return True
    except Exception as exc:  # noqa: BLE001
        log(f"[approval] snapshot post failed: {exc!r}")
        return False


def handle_approval(payload: dict):
    """Gate a PreToolUse on the device.

    Returns 'approve' | 'deny' | 'dismiss', or None when the tool isn't gated or
    the device is unreachable (caller then proceeds with the normal flow). On a
    real decision it has already emitted the PreToolUse permissionDecision.
    """
    if DRY_RUN:
        return None
    tool = payload.get("tool_name", "") or "tool"
    if not _tool_gated(tool):
        return None

    req_id = os.urandom(4).hex()
    hint = _approval_hint(payload)
    pending = {
        "running": 0, "waiting": 1, "total": 0,
        "msg": f"approve {tool}?"[:48],
        "prompt": {"id": req_id, "tool": tool, "hint": hint},
        "entries": [f"? {tool}: {hint}"[:48]],
        "source": "approval",
    }
    # Reachability test: if we can't post the prompt, don't block — normal flow.
    if not _post_snapshot(pending):
        log("[approval] device unreachable; deferring to terminal")
        return None

    deadline = time.time() + APPROVAL_TIMEOUT
    decision = "dismiss"
    while time.time() < deadline:
        try:
            resp = _http(f"{BASE_URL}/decision?id={req_id}")
            d = json.loads(resp).get("decision", "pending")
        except Exception:  # noqa: BLE001
            d = "pending"
        if d in ("approve", "deny", "dismiss"):
            decision = d
            break
        time.sleep(APPROVAL_POLL)

    if decision == "approve":
        # Let the normal PreToolUse flow update counters / clear the prompt.
        _emit_pretool_decision("allow", f"approved {tool} on claudi")
    elif decision == "deny":
        _emit_pretool_decision("deny", f"denied {tool} on claudi")
        _post_snapshot({"running": 0, "waiting": 0, "total": 0,
                        "msg": f"denied {tool}"[:48], "source": "denied"})
    else:
        _emit_pretool_decision("ask", "dismissed on claudi; use the terminal prompt")
        _post_snapshot({"running": 0, "waiting": 0, "total": 0,
                        "msg": "dismissed", "source": "dismiss"})
    log(f"[approval] {tool} -> {decision} (req {req_id})")
    return decision


def run_from_stdin() -> int:
    raw = ""
    try:
        raw = sys.stdin.read()
    except Exception as exc:  # noqa: BLE001
        log(f"[error] reading stdin: {exc!r}")

    payload: dict = {}
    if raw.strip():
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError as exc:
            log(f"[error] bad JSON on stdin: {exc!r}; raw={raw[:200]!r}")

    if not ENABLED:
        log("[skip] CLAUDI_ENABLED is false")
        return 0

    event = payload.get("hook_event_name", "") or payload.get("hook_event", "") or "?"
    session_id = payload.get("session_id", "") or ""

    # On-device approval gate (opt-in). For a denied/dismissed tool the decision
    # is final and we skip the normal snapshot; for an approved tool we fall
    # through so counters update and the prompt clears as usual.
    if event == "PreToolUse" and APPROVAL_ENABLED:
        try:
            decision = handle_approval(payload)
        except Exception as exc:  # noqa: BLE001 - never break the tool call
            log(f"[approval] error: {exc!r}; deferring to terminal")
            decision = None
        if decision in ("deny", "dismiss"):
            return 0

    with _state_lock():
        state = load_state(session_id)
        overlay, overlay_ms = apply_event(state, payload)
        save_state(state)

    tool = payload.get("tool_name", "")
    log(f"[event] {event} tool='{tool}' running={state.get('running')} "
        f"waiting={state.get('waiting')} overlay={overlay or '-'}")
    send_snapshot(state, event, overlay, overlay_ms)
    return 0


def self_test() -> int:
    """Exercise a realistic event sequence end-to-end in dry-run (no hardware).

    Uses an isolated temp state file so it never touches the real session state.
    Asserts: running never goes negative, derived state always valid, and the
    waiting/running ladder behaves as expected.
    """
    global DRY_RUN, STATE_FILE
    DRY_RUN = True
    STATE_FILE = os.path.join(_HERE, ".claudi_selftest_state.json")
    for p in (STATE_FILE, STATE_FILE + ".tmp", STATE_FILE + ".lock"):
        with contextlib.suppress(OSError):
            os.remove(p)

    print(f"claudi hook self-test (base={BASE_URL}, dry-run, isolated state)\n")
    sequence = [
        ({"hook_event_name": "SessionStart", "session_id": "s1"}, None),
        ({"hook_event_name": "UserPromptSubmit", "session_id": "s1", "prompt": "build it"}, "idle"),
        ({"hook_event_name": "PreToolUse", "session_id": "s1", "tool_name": "Read"}, "working"),
        ({"hook_event_name": "PostToolUse", "session_id": "s1", "tool_name": "Read"}, "idle"),
        ({"hook_event_name": "PreToolUse", "session_id": "s1", "tool_name": "Bash"}, "working"),
        ({"hook_event_name": "PreToolUse", "session_id": "s1", "tool_name": "Edit"}, "working"),
        ({"hook_event_name": "PostToolUse", "session_id": "s1", "tool_name": "Edit"}, "working"),
        ({"hook_event_name": "PostToolUse", "session_id": "s1", "tool_name": "Bash"}, "idle"),
        ({"hook_event_name": "Notification", "session_id": "s1", "message": "approve Bash?"}, "attention"),
        ({"hook_event_name": "Stop", "session_id": "s1"}, "idle"),
        # New session id must reset cleanly:
        ({"hook_event_name": "PreToolUse", "session_id": "s2", "tool_name": "Bash"}, "working"),
        ({"hook_event_name": "SessionEnd", "session_id": "s2"}, "idle"),
    ]
    ok = True
    for payload, expect in sequence:
        with _state_lock():
            state = load_state(payload.get("session_id", ""))
            overlay, _ = apply_event(state, payload)
            save_state(state)
        derived = derive_state(state)
        running = int(state.get("running", 0))
        bad = []
        if running < 0:
            bad.append("running<0")
        if derived not in VALID_STATES:
            bad.append("bad-derived")
        if expect is not None and derived != expect:
            bad.append(f"want {expect}")
        mark = "ok" if not bad else "FAIL"
        if bad:
            ok = False
        ev = payload["hook_event_name"]
        tool = payload.get("tool_name", "")
        label = f"{ev}{('/' + tool) if tool else ''}"
        print(f"  [{mark}] {label:<26} run={running} wait={int(bool(state.get('waiting')))} "
              f"-> base={derived:<10} overlay={overlay or '-':<8} {' '.join(bad)}")

    for p in (STATE_FILE, STATE_FILE + ".tmp", STATE_FILE + ".lock"):
        with contextlib.suppress(OSError):
            os.remove(p)
    print("\nself-test:", "PASS" if ok else "FAILED")
    return 0 if ok else 1


def main(argv: list[str]) -> int:
    global DRY_RUN
    if "--self-test" in argv:
        return self_test()
    if "--dry-run" in argv:
        DRY_RUN = True
    if "--ping" in argv:
        print(f"pinging {BASE_URL} ...")
        snap_state = _fresh_state("ping")
        snap_state["msg"] = "manual ping"
        ok = send_snapshot(snap_state, "ping", "happy", 2000)
        print("reachable" if ok else "UNREACHABLE (see log)")
        return 0 if ok else 1
    return run_from_stdin()


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except Exception as exc:  # noqa: BLE001 - last-resort safety net
        log(f"[fatal] uncaught: {exc!r}")
        sys.exit(0)
