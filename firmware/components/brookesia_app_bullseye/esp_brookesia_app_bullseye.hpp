/*
 * Bullseye — a reflex/timing game for the round AMOLED, shipped as an
 * ESP-Brookesia phone app (self-registers into the launcher).
 *
 * A target ring sits at a random spot on the screen; an "approach" ring spawns
 * large and shrinks down onto it. Tap when the approach ring lines up with the
 * target to score — the closer the match, the more points. Three misses (a ring
 * collapses untapped, or you tap way too early) ends the run. The longer you
 * survive, the faster the rings shrink.
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class BullseyeApp: public systems::phone::App {
public:
    static BullseyeApp *requestInstance();
    ~BullseyeApp();

protected:
    BullseyeApp();

    // Build the UI on the default screen (lv_scr_act()).
    bool run(void) override;
    // Back gesture: leave the game, return to launcher.
    bool back(void) override;

private:
    static BullseyeApp *_instance;

    enum class Phase { Countdown, Playing, GameOver };

    // ~60fps game loop driving the shrinking ring + state machine.
    static void onTick(lv_timer_t *timer);
    void tick(void);

    // Screen tap (anywhere). During play it's a hit attempt; on the game-over
    // screen it restarts; during the countdown it's ignored.
    static void onPress(lv_event_t *e);
    void handlePress(void);

    void resetGame(void);       // back to a fresh countdown
    void spawnCircle(void);     // place a new target+approach ring at random
    void registerHit(int points, const char *label, uint32_t color);
    void registerMiss(const char *label);
    void enterGameOver(void);

    float approachRadius(uint32_t now_us) const;  // current ring radius
    void layoutCircle(lv_obj_t *obj, float radius); // size+center a ring object

    // Widgets (all created in run(); recycled by the core on app close).
    lv_obj_t *_target = nullptr;     // fixed center ring (the bullseye)
    lv_obj_t *_approach = nullptr;   // shrinking ring
    lv_obj_t *_score_label = nullptr;
    lv_obj_t *_life_pip[3] = {nullptr, nullptr, nullptr};
    lv_obj_t *_feedback = nullptr;   // "PERFECT +300" / "Miss" flashes
    lv_obj_t *_big = nullptr;        // countdown digits / "GAME OVER"
    lv_timer_t *_timer = nullptr;

    // Game state.
    Phase _phase = Phase::Countdown;
    int _score = 0;
    int _lives = 3;
    int _hits = 0;                   // difficulty counter (rings shrink faster)
    bool _circle_active = false;
    int _cx = 233, _cy = 233;        // current ring center
    uint32_t _circle_start_us = 0;   // when the active ring spawned
    uint32_t _circle_duration_ms = 0;
    uint32_t _countdown_start_us = 0;
    uint32_t _next_spawn_us = 0;     // gap before the next ring
    uint32_t _feedback_until_us = 0; // when to clear the feedback flash
    int _last_countdown_shown = -1;
};

} // namespace esp_brookesia::apps
