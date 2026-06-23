/*
 * Pixel Reflex — a fast single-target reaction game, shipped as an ESP-Brookesia
 * phone app (self-registers into the launcher).
 *
 * A 3x3 grid of chunky square "pixel" cells fills the center of the round screen.
 * All cells sit dim except for exactly ONE that lights up a bright retro color.
 * Tap the lit cell as fast as you can: you score +1, your reaction time is shown
 * in milliseconds, a new (never-repeating) cell lights up, and the per-target
 * timeout shrinks a little — so the game speeds up the better you do. Taps on dim
 * cells are forgiving and ignored. If the lit cell's countdown runs out before you
 * tap it, it's GAME OVER: the final score, your best score, and "tap to retry" are
 * shown, and a tap anywhere starts a fresh game.
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class PixelReflexApp: public systems::phone::App {
public:
    static PixelReflexApp *requestInstance();
    ~PixelReflexApp();

protected:
    PixelReflexApp();

    // Build the UI on the default screen (lv_scr_act()).
    bool run(void) override;
    // Back gesture: leave the game, return to launcher.
    bool back(void) override;

private:
    static PixelReflexApp *_instance;

    static constexpr int GRID = 3;            // 3x3 grid
    static constexpr int CELLS = GRID * GRID;  // 9 cells

    enum class Phase { Playing, GameOver };

    // ~30ms game loop: drives the lit-cell timeout and the shrinking time-bar.
    static void onTick(lv_timer_t *timer);
    void tick(void);

    // A grid cell was tapped (LV_EVENT_CLICKED on the cell object).
    static void onCellClick(lv_event_t *e);
    // Anywhere on the screen was pressed — only restarts during GameOver.
    static void onScreenPress(lv_event_t *e);
    void handleScreenPress(void);

    void resetGame(void);                // start a fresh game
    void lightNewCell(void);             // pick + light a fresh random target
    void setCellLit(int idx, bool lit);  // recolor a cell dim/bright
    void enterGameOver(void);

    // Current timeout (ms) for the lit cell — shrinks as the score climbs.
    uint32_t currentTimeoutMs(void) const;

    // The 9 cell blocks. Created once in run() and recycled (recolored) only.
    lv_obj_t *_cells[CELLS] = {};

    // HUD widgets (created in run()).
    lv_obj_t *_score_label = nullptr;  // big score, top center
    lv_obj_t *_rt_label = nullptr;     // reaction time "123 ms", under the score
    lv_obj_t *_timebar = nullptr;      // thin shrinking time-bar near the bottom
    lv_obj_t *_over_label = nullptr;   // centered game-over summary
    lv_timer_t *_timer = nullptr;

    // Game state.
    Phase _phase = Phase::Playing;
    int _score = 0;
    int _lit = -1;             // index of the currently-lit cell (-1 = none)
    int _prev_lit = -1;        // last lit cell, so we never repeat back-to-back
    uint32_t _lit_color = 0;   // bright color used for the current target
    uint32_t _lit_start_us = 0;  // when the current target lit up
    uint32_t _lit_timeout_ms = 0;  // timeout for the current target
};

} // namespace esp_brookesia::apps
