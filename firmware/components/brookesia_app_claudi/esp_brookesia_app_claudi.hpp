/*
 * claudi — an ESP-Brookesia phone app that mirrors Claude Code activity as a
 * reactive pet + transcript/approval HUD on the AMOLED.
 *
 * The app pulls the latest activity snapshot from claudi_net (filled by the
 * Mac-side hook over HTTP) and runs the portable claudi_core ladder to pick the
 * pet state. V1 renders the pet programmatically (state-colored animated blob);
 * GIF art on LittleFS is a later drop-in.
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class ClaudiApp: public systems::phone::App {
public:
    static ClaudiApp *requestInstance();
    ~ClaudiApp();

protected:
    ClaudiApp();

    // Build all UI on app start (drawn on the default screen, lv_scr_act()).
    bool run(void) override;
    // Back gesture: close the app (returns to launcher).
    bool back(void) override;

private:
    static ClaudiApp *_instance;

    // Periodic refresh: read snapshot → derive → update widgets. Static so it
    // can be used as an lv_timer callback; recovers `this` from user_data.
    static void onTick(lv_timer_t *timer);
    void applyState(void);

    // Lay `text` along the top arc (round-screen status). Rebuilds the per-char
    // labels only when the text or colour changes.
    static constexpr int kMaxArcChars = 28;
    void setArcText(const char *text, uint32_t color);

    // Widgets (created in run(); auto-recycled by the core on close).
    lv_obj_t *_pet = nullptr;        // lv_animimg playing the per-state slime art
    lv_obj_t *_ring = nullptr;       // perimeter ring, coloured by state
    lv_obj_t *_arc_chars[kMaxArcChars] = {nullptr};  // curved status text (state word)
    lv_obj_t *_wifi_bubble = nullptr; // floating Wi-Fi bubble
    lv_obj_t *_wifi_label = nullptr;
    lv_obj_t *_batt_bubble = nullptr; // floating battery bubble
    lv_obj_t *_batt_label = nullptr;
    lv_obj_t *_sess_bubble = nullptr; // floating live-session-count bubble
    lv_obj_t *_sess_label = nullptr;
    lv_obj_t *_transcript = nullptr; // bottom status line
    lv_obj_t *_card = nullptr;       // approval card container
    lv_obj_t *_card_label = nullptr; // approval card text
    lv_timer_t *_timer = nullptr;

    char _arc_cache[48] = "";        // last arc string (avoid rebuilds)
    uint32_t _arc_color = 0;         // last arc colour
    char _card_cache[160] = "";      // last card text (avoid per-tick relayout)
    int _last_state = -1;            // last effective state (avoid redundant work)
    int _last_sessions = -1;         // last session count (gate bubble updates)
    uint32_t _attention_since_ms = 0;  // for the ~10s approval escalation
    uint32_t _last_batt_ms = 0;      // throttle battery polling (I2C)
    int _batt_pct = -1;              // cached battery percent
    bool _charging = false;          // cached charge state
};

} // namespace esp_brookesia::apps
