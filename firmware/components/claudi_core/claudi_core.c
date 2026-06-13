// claudi_core.c — see claudi_core.h. Pure logic, no hardware/IDF dependencies.
#include "claudi_core.h"

#include <string.h>

// Index-aligned with claudi_state_t (so name[st] is O(1)). Keep in lockstep
// with the enum; the static_assert below guards against drift.
static const char *const kStateNames[CLAUDI_STATE_COUNT] = {
    [CLAUDI_STATE_IDLE]      = "idle",
    [CLAUDI_STATE_BLINK]     = "blink",
    [CLAUDI_STATE_HAPPY]     = "happy",
    [CLAUDI_STATE_SLEEPY]    = "sleepy",
    [CLAUDI_STATE_CURIOUS]   = "curious",
    [CLAUDI_STATE_ALERT]     = "alert",
    [CLAUDI_STATE_BORED]     = "bored",
    [CLAUDI_STATE_WORKING]   = "working",
    [CLAUDI_STATE_THINKING]  = "thinking",
    [CLAUDI_STATE_ATTENTION] = "attention",
    [CLAUDI_STATE_IDEA]      = "idea",
    [CLAUDI_STATE_EXCITED]   = "excited",
};

void claudi_snapshot_init(claudi_snapshot_t *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->overlay = CLAUDI_STATE_COUNT;  // none
}

static void copy_truncated(char *dst, size_t dst_sz, const char *src)
{
    if (dst_sz == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_sz) {
        n = dst_sz - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void claudi_snapshot_add_entry(claudi_snapshot_t *s, const char *text)
{
    if (s == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    if (s->n_entries < CLAUDI_MAX_ENTRIES) {
        copy_truncated(s->entries[s->n_entries], CLAUDI_ENTRY_LEN, text);
        s->n_entries++;
        return;
    }
    // Full: drop the oldest, shift down, append at the end (newest last).
    for (size_t i = 1; i < CLAUDI_MAX_ENTRIES; ++i) {
        memcpy(s->entries[i - 1], s->entries[i], CLAUDI_ENTRY_LEN);
    }
    copy_truncated(s->entries[CLAUDI_MAX_ENTRIES - 1], CLAUDI_ENTRY_LEN, text);
}

const char *claudi_state_name(claudi_state_t st)
{
    if (st < 0 || st >= CLAUDI_STATE_COUNT) {
        return "idle";
    }
    return kStateNames[st];
}

bool claudi_state_from_name(const char *name, claudi_state_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (int i = 0; i < CLAUDI_STATE_COUNT; ++i) {
        if (strcmp(name, kStateNames[i]) == 0) {
            *out = (claudi_state_t)i;
            return true;
        }
    }
    return false;
}

claudi_derived_t claudi_derive(const claudi_snapshot_t *s, uint32_t now_ms)
{
    claudi_derived_t d;
    d.base = CLAUDI_STATE_IDLE;
    d.overlay = CLAUDI_STATE_COUNT;
    d.effective = CLAUDI_STATE_IDLE;
    d.stale = false;
    d.overlay_active = false;

    if (s == NULL) {
        return d;
    }

    // Elapsed since the snapshot was applied, with unsigned wraparound safety.
    uint32_t elapsed = now_ms - s->updated_ms;
    d.stale = elapsed > CLAUDI_STALE_MS;

    // Priority ladder (mirrors the hook's derive_state plus firmware staleness):
    //   attention > working > sleepy(stale) > idle
    // A pending approval outranks staleness so the pet keeps asking; a tool in
    // flight outranks staleness so long builds stay "working".
    if (s->waiting || s->prompt.set) {
        d.base = CLAUDI_STATE_ATTENTION;
    } else if (s->running > 0) {
        d.base = CLAUDI_STATE_WORKING;
    } else if (d.stale) {
        d.base = CLAUDI_STATE_SLEEPY;
    } else {
        d.base = CLAUDI_STATE_IDLE;
    }

    // One-shot overlay: a transient flavour that auto-reverts after overlay_ms.
    if (s->overlay >= 0 && s->overlay < CLAUDI_STATE_COUNT && s->overlay_ms > 0 &&
        elapsed < s->overlay_ms) {
        d.overlay = s->overlay;
        d.overlay_active = true;
        d.effective = s->overlay;
    } else {
        d.effective = d.base;
    }

    return d;
}
