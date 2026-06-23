/*
 * Pixel 2048 — the classic 2048 slide-and-merge puzzle, rendered in a crisp retro
 * block style and shipped as an ESP-Brookesia phone app (self-registers into the
 * launcher).
 *
 * A 4x4 grid of square tiles fills the round play area. Swipe (flick) in any of the
 * four directions to slide every tile that way; equal neighbours merge once per move
 * into their sum, adding to the score. Each move that changes the board spawns a new
 * 2 (90%) or 4 (10%) in a random empty cell. Reaching 2048 flashes a one-shot
 * "2048!" banner but play continues. When the board is full with no possible merge,
 * a "Game Over" overlay shows the score and a tap anywhere starts a fresh game.
 *
 * Input is SWIPE: the press-start point is recorded on LV_EVENT_PRESSED and the move
 * direction is derived from the press-start vs release delta on LV_EVENT_RELEASED.
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class Pixel2048App: public systems::phone::App {
public:
    static Pixel2048App *requestInstance();
    ~Pixel2048App();

protected:
    Pixel2048App();

    // Build the UI on the default screen (lv_scr_act()).
    bool run(void) override;
    // Back gesture: leave the game, return to launcher.
    bool back(void) override;

private:
    static Pixel2048App *_instance;

    static constexpr int N = 4;          // 4x4 board
    static constexpr int CELLS = N * N;  // 16 tiles

    enum class Phase { Playing, GameOver };

    // Swipe direction of a move.
    enum class Dir { Left, Right, Up, Down };

    // ~30ms loop: only refreshes transient UI (the one-shot "2048!" banner timing);
    // the board logic itself runs on swipe (press/release) events.
    static void onTick(lv_timer_t *timer);
    void tick(void);

    // Swipe detection on the whole screen.
    static void onPressed(lv_event_t *e);   // record the flick start point
    static void onReleased(lv_event_t *e);  // resolve direction, do a move
    void handlePressed(int x, int y);
    void handleReleased(int x, int y);

    void resetGame(void);            // start a fresh game
    void render(void);               // paint all 16 tiles from the board
    bool doMove(Dir dir);            // slide+merge; returns true if board changed
    void spawnTile(void);            // add a 2/4 into a random empty cell
    bool hasMove(void) const;        // any empty cell or adjacent equal pair?
    void enterGameOver(void);

    // Map a board value to its tile background colour.
    static uint32_t tileColor(int value);

    // 16 recycled tile blocks (created once in run()), each with a number label.
    lv_obj_t *_tiles[CELLS] = { nullptr };
    lv_obj_t *_tileLabels[CELLS] = { nullptr };

    // HUD + overlay widgets (created in run()).
    lv_obj_t *_score_label = nullptr;   // score, top center
    lv_obj_t *_banner = nullptr;        // one-shot "2048!" flash
    lv_obj_t *_over = nullptr;          // "Game Over" overlay

    lv_timer_t *_timer = nullptr;

    // Game state.
    Phase _phase = Phase::Playing;
    int _board[CELLS] = { 0 };   // row-major; 0 == empty
    int _score = 0;
    bool _won = false;            // 2048 banner already shown this game
    uint32_t _banner_until_us = 0;

    // Flick tracking: the touch point recorded on LV_EVENT_PRESSED.
    int _press_x = 0;
    int _press_y = 0;
};

} // namespace esp_brookesia::apps
