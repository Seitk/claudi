/*
 * Color Match — a Simon-style color memory game for kids, shipped as an
 * ESP-Brookesia phone app (self-registers into the launcher). Round 466x466 AMOLED.
 *
 * Four big rounded pads in a 2x2 layout light up in a growing sequence; the kid
 * watches, then taps them back in order. Get the whole sequence right and the
 * level (and score) goes up and the sequence grows. A wrong tap is gentle: a soft
 * "Oops! Watch again", then the SAME sequence replays so they can try the level
 * again. No hard game over — kids just keep playing and chasing their best level.
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class ColorMatchApp: public systems::phone::App {
public:
    static ColorMatchApp *requestInstance();
    ~ColorMatchApp();

protected:
    ColorMatchApp();

    // Build the UI on the default screen (lv_scr_act()).
    bool run(void) override;
    // Back gesture: leave the game, return to launcher.
    bool back(void) override;

private:
    static ColorMatchApp *_instance;

    static constexpr int PAD_COUNT = 4;     // four colored pads
    static constexpr int SEQ_MAX = 32;      // sequence length cap

    enum class Phase { Idle, Showing, Input, Celebrate, Oops };

    // ~30ms game loop driving sequence playback + flash timing via timestamps.
    static void onTick(lv_timer_t *timer);
    void tick(void);

    // A pad was tapped (during Input). The pad index 0..3 is the user data.
    static void onPadPress(lv_event_t *e);
    void handlePadPress(int pad);

    // Center prompt tapped at boot ("tap to start").
    static void onPromptPress(lv_event_t *e);

    void resetGame(void);        // back to level 1 with a fresh 1-step sequence
    void startRound(void);       // (re)begin playback of the current sequence
    void extendSequence(void);   // append a random pad, grow the level
    void enterInput(void);       // hand control to the kid
    void enterCelebrate(void);   // brief "Nice!" before the next round
    void enterOops(void);        // gentle wrong-tap feedback, then replay same seq

    void setPadLit(int pad, bool lit);   // bright vs dim
    void flashPad(int pad, uint32_t now);// light a pad briefly during Input
    void updateLevelLabel(void);

    // Widgets (all created in run(); recycled by the core on app close).
    lv_obj_t *_pad[PAD_COUNT] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t *_level_label = nullptr;    // "Level N" at top
    lv_obj_t *_status_label = nullptr;   // "Watch!" / "Your turn!" / etc.
    lv_obj_t *_prompt = nullptr;         // big center "Tap to start" at boot
    lv_timer_t *_timer = nullptr;

    // Game state.
    Phase _phase = Phase::Idle;
    int _seq[SEQ_MAX] = {0};
    int _seq_len = 0;            // current sequence length
    int _level = 1;             // also the score; highest reached is tracked
    int _best = 1;

    // Playback (Showing) cursor.
    int _play_index = 0;        // which step of the sequence is showing
    bool _play_on = false;      // is the current pad currently lit?
    uint32_t _play_phase_until_us = 0;  // when the current lit/gap window ends

    // Input cursor.
    int _input_index = 0;       // next expected step
    int _flash_pad = -1;        // pad currently flashing from a tap
    uint32_t _flash_until_us = 0;

    // Celebrate / Oops timing.
    uint32_t _phase_until_us = 0;
};

} // namespace esp_brookesia::apps
