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

// Start Wi-Fi STA, mDNS, and the HTTP server. Safe to call once after the
// display/Brookesia are up. Non-blocking: connection proceeds in the background.
void claudi_net_start(void);

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
