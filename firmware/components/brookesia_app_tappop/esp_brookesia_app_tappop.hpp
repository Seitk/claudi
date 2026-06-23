/*
 * Tap Pop — a gentle whack-a-mole / tap-the-bubble reflex game for young kids,
 * shipped as an ESP-Brookesia phone app (self-registers into the launcher).
 *
 * Up to 3 friendly "poppers" (bright filled circles with cute faces) appear at
 * random spots inside the round play area. Tap one before it disappears to score
 * a point and pop it with a happy "+1" sparkle. It's a forgiving, failure-free
 * 30-second round: when the clock runs out, a "Great job!" celebration shows and
 * tapping anywhere starts a fresh round.
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class TapPopApp: public systems::phone::App {
public:
    static TapPopApp *requestInstance();
    ~TapPopApp();

protected:
    TapPopApp();

    // Build the UI on the default screen (lv_scr_act()).
    bool run(void) override;
    // Back gesture: leave the game, return to launcher.
    bool back(void) override;

private:
    static TapPopApp *_instance;

    static constexpr int SLOTS = 3;  // up to 3 poppers visible at once

    enum class Phase { Playing, GameOver };

    // ~30ms game loop: drives spawns, lifetimes, and the round clock.
    static void onTick(lv_timer_t *timer);
    void tick(void);

    // A popper was tapped (LV_EVENT_CLICKED on its object).
    static void onPopperClick(lv_event_t *e);
    // Anywhere on the screen was pressed — only restarts during GameOver.
    static void onScreenPress(lv_event_t *e);
    void handleScreenPress(void);

    void resetGame(void);                 // start a fresh 30s round
    void spawnPopper(int slot, uint32_t now); // show a slot at a random spot
    void hidePopper(int slot);            // tuck a slot away
    void popSlot(int slot);               // a tapped popper: score + sparkle
    void enterGameOver(void);

    // Per-slot popper widgets and state. Objects are created once in run() and
    // recycled by show/hide — never created/destroyed per spawn.
    struct Slot {
        lv_obj_t *body = nullptr;  // the colored circle (clickable)
        lv_obj_t *eyeL = nullptr;  // left eye dot
        lv_obj_t *eyeR = nullptr;  // right eye dot
        lv_obj_t *smile = nullptr; // tiny mouth bar
        bool active = false;
        uint32_t spawn_us = 0;
        uint32_t life_ms = 0;
        uint32_t next_spawn_us = 0; // when an empty slot may spawn again
    };
    Slot _slots[SLOTS];

    // HUD + celebration widgets (created in run()).
    lv_obj_t *_score_label = nullptr;  // big score, top center
    lv_obj_t *_time_label = nullptr;   // seconds remaining, small
    lv_obj_t *_spark = nullptr;        // "+1" pop feedback flash
    lv_obj_t *_big = nullptr;          // "Great job!" celebration banner
    lv_timer_t *_timer = nullptr;

    // Game state.
    Phase _phase = Phase::Playing;
    int _score = 0;
    uint32_t _round_start_us = 0;
    uint32_t _spark_until_us = 0;
    int _last_secs_shown = -1;
};

} // namespace esp_brookesia::apps
