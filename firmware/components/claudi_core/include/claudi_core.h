// claudi_core.h — portable, hardware-independent core of the claudi companion.
//
// This module owns the *behaviour* of claudi: the activity snapshot pushed by
// the Mac-side Claude Code hook (`.claude/hooks/claudi_hook.py`) and the
// derivation of a pet state from it. It has NO dependency on ESP-IDF, LVGL, or
// any hardware, so it compiles and unit-tests on the host (see test/).
//
// The device firmware feeds this module (the HTTP ingest layer parses JSON into
// a claudi_snapshot_t) and renders the result of claudi_derive().
//
// State model mirrors the hook's `derive_state()` plus firmware-side staleness:
//
//   waiting>0 OR prompt set → attention   (highest priority: needs the human)
//   running>0               → working
//   stale (no snapshot 30s) → sleepy
//   otherwise               → idle
//
// A transient one-shot `overlay` (idea/curious/happy/…) layers on top for
// `overlay_ms` after the snapshot, then auto-reverts to the derived base so a
// reaction never sticks.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLAUDI_MAX_ENTRIES 5    // recent transcript lines kept (matches hook)
#define CLAUDI_ENTRY_LEN   49   // 48 chars + NUL (hook truncates entries to 48)
#define CLAUDI_MSG_LEN     49   // 48 chars + NUL
#define CLAUDI_ID_LEN      32
#define CLAUDI_TOOL_LEN    32
#define CLAUDI_HINT_LEN    64

#define CLAUDI_STALE_MS    30000u  // no snapshot for this long → sleepy

// The full pet-state vocabulary. Order is the stable wire/index order; keep in
// sync with claudi_state_name()/claudi_state_from_name() and the renderer's
// asset table. CLAUDI_STATE_COUNT doubles as the "no overlay" sentinel.
typedef enum {
    CLAUDI_STATE_IDLE = 0,
    CLAUDI_STATE_BLINK,
    CLAUDI_STATE_HAPPY,
    CLAUDI_STATE_SLEEPY,
    CLAUDI_STATE_CURIOUS,
    CLAUDI_STATE_ALERT,
    CLAUDI_STATE_BORED,
    CLAUDI_STATE_WORKING,
    CLAUDI_STATE_THINKING,
    CLAUDI_STATE_ATTENTION,
    CLAUDI_STATE_IDEA,
    CLAUDI_STATE_EXCITED,
    CLAUDI_STATE_COUNT,        // == "none"; not a real state
} claudi_state_t;

// A pending approval/input request surfaced by the hook's Notification event.
typedef struct {
    bool set;
    char id[CLAUDI_ID_LEN];
    char tool[CLAUDI_TOOL_LEN];
    char hint[CLAUDI_HINT_LEN];
} claudi_prompt_t;

// The aggregated session snapshot. The ingest layer fills this from the hook's
// POST /snapshot JSON; the host tests fill it directly.
typedef struct {
    int32_t  total;        // cumulative tool calls (summed across sessions)
    int32_t  running;      // tools currently in flight (summed across sessions)
    int32_t  sessions;     // number of live Claude Code sessions
    bool     waiting;      // blocked on the human (approval / input)
    char     msg[CLAUDI_MSG_LEN];                       // latest one-line status
    char     entries[CLAUDI_MAX_ENTRIES][CLAUDI_ENTRY_LEN];  // newest last
    size_t   n_entries;
    claudi_prompt_t prompt;

    claudi_state_t overlay;     // transient flavour, or CLAUDI_STATE_COUNT
    uint32_t       overlay_ms;  // how long the overlay stays active
    uint32_t       updated_ms;  // monotonic ms when this snapshot was applied
} claudi_snapshot_t;

// The result of evaluating a snapshot at a point in time.
typedef struct {
    claudi_state_t base;       // ladder result, ignoring overlay
    claudi_state_t overlay;    // active overlay, or CLAUDI_STATE_COUNT if none
    claudi_state_t effective;  // what the renderer should show now
    bool stale;                // no snapshot within CLAUDI_STALE_MS
    bool overlay_active;       // overlay currently showing
} claudi_derived_t;

// Reset a snapshot to the clean/idle baseline (overlay = none).
void claudi_snapshot_init(claudi_snapshot_t *s);

// Append a transcript entry, truncating to CLAUDI_ENTRY_LEN and keeping only the
// most recent CLAUDI_MAX_ENTRIES (oldest dropped). NULL/empty text is ignored.
void claudi_snapshot_add_entry(claudi_snapshot_t *s, const char *text);

// Stable lowercase name for a state (e.g. "working"). Returns "idle" for
// CLAUDI_STATE_COUNT or any out-of-range value.
const char *claudi_state_name(claudi_state_t st);

// Parse a state name into *out. Returns false (and leaves *out untouched) if the
// name is unknown — used to validate overlays and the legacy /pet/state path.
bool claudi_state_from_name(const char *name, claudi_state_t *out);

// Evaluate the snapshot at now_ms (a monotonic millisecond clock). Pure: no
// global state, no I/O. now_ms must be >= s->updated_ms in normal operation;
// elapsed time is computed with unsigned wraparound semantics.
claudi_derived_t claudi_derive(const claudi_snapshot_t *s, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
