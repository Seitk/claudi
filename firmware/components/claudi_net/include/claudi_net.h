// claudi_net.h — Wi-Fi + mDNS + HTTP ingest for the claudi companion.
//
// Brings up Wi-Fi STA, advertises mDNS `claudi.local`, and runs an HTTP server
// that speaks exactly the protocol the Mac-side hook (claudi_hook.py) emits:
//
//   POST /snapshot          body = the hook's snapshot JSON  → updates the model
//   GET  /status            → JSON: derived state + system/network info
//   GET  /pet/state?state=  → legacy fallback: force a state (manual/curl)
//   GET  /render?...        → legacy no-op (logged)
//
// The parsed snapshot is held thread-safe; the UI polls it via
// claudi_net_get_snapshot() and runs claudi_derive() to drive the pet.
#pragma once

#include "claudi_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Approval decision values for the device button/touch return-channel.
// Reported by GET /decision and set by the UI via claudi_net_post_decision().
enum {
    CLAUDI_DECISION_PENDING = 0,
    CLAUDI_DECISION_APPROVE,
    CLAUDI_DECISION_DENY,
    CLAUDI_DECISION_DISMISS,
};

// Start Wi-Fi STA, mDNS, and the HTTP server. Safe to call once after the
// display/Brookesia are up. Non-blocking: connection proceeds in the background.
void claudi_net_start(void);

// True if an approval request is currently shown and awaiting a decision.
bool claudi_net_pending(void);

// Record the user's decision for the pending approval (CLAUDI_DECISION_*).
// No-op if nothing is pending. Called from the button/touch handlers; the
// polling hook reads the result via GET /decision.
void claudi_net_post_decision(int decision);

// Copy the current snapshot under lock. `out` must be non-NULL.
void claudi_net_get_snapshot(claudi_snapshot_t *out);

// True once the STA has an IP.
bool claudi_net_is_connected(void);

// Current STA IPv4 as a string ("0.0.0.0" until connected). Valid until the
// next connection change; copy if you need to keep it.
const char *claudi_net_ip(void);

#ifdef __cplusplus
}
#endif
