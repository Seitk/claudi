/*
 * Pixel Snake — classic Snake on a chunky pixel grid, shipped as an
 * ESP-Brookesia phone app (self-registers into the launcher).
 *
 * A 15x15 grid of 28px square cells fills the round screen. A green snake
 * (brighter head) auto-advances one cell per step; you steer it by dragging
 * a finger — the snake turns toward wherever you're touching (dominant axis,
 * no 180 reversals). Eat the red food block to grow and score; hit a wall or
 * your own body and it's GAME OVER, after which a tap anywhere replays. The
 * snake speeds up gently as it grows. Best score persists across launches.
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class PixelSnakeApp: public systems::phone::App {
public:
    static PixelSnakeApp *requestInstance();
    ~PixelSnakeApp();

protected:
    PixelSnakeApp();

    // Build the UI on the default screen (lv_scr_act()).
    bool run(void) override;
    // Back gesture: leave the game, return to launcher.
    bool back(void) override;

private:
    static PixelSnakeApp *_instance;

    static constexpr int GRID = 15;          // 15x15 cells
    static constexpr int CELL = 28;          // px per cell (15*28 = 420)
    static constexpr int OFFSET = 23;        // (466-420)/2, top-left of the board
    static constexpr int MAX_LEN = GRID * GRID;   // snake can fill the board
    static constexpr int POOL = 64;          // recycled segment block pool
    static constexpr int START_LEN = 3;      // starting body length

    enum class Phase { Playing, GameOver };
    enum class Dir { Up, Down, Left, Right };

    // 16ms game loop: accumulates time and steps the snake when due.
    static void onTick(lv_timer_t *timer);
    void tick(void);

    // Whole-screen touch: PRESSED/PRESSING track the steer point; PRESSED also
    // restarts during GameOver. RELEASE keeps the last touch point (and dir).
    static void onPressed(lv_event_t *e);
    static void onPressing(lv_event_t *e);
    void readTouch(void);          // latch the live touch point

    void resetGame(void);          // start a fresh round
    void step(void);               // advance the snake one cell
    void applySteer(void);         // turn toward the latched touch point
    void spawnFood(void);          // place food at a random empty cell
    void render(void);             // sync block pool to snake + food
    void enterGameOver(void);

    bool occupied(int gx, int gy) const;  // is a snake cell at (gx,gy)?

    // Grid cell -> pixel center (used for steering math).
    int cellCx(int gx) const { return OFFSET + gx * CELL + CELL / 2; }
    int cellCy(int gy) const { return OFFSET + gy * CELL + CELL / 2; }

    // Recycled square blocks: index 0..len-1 are the snake body, the rest hide.
    lv_obj_t *_blocks[POOL] = {nullptr};
    lv_obj_t *_food_block = nullptr;        // the red food square

    // HUD + game-over banner (created in run()).
    lv_obj_t *_score_label = nullptr;       // length, small top-center
    lv_obj_t *_big = nullptr;               // centered GAME OVER text
    lv_timer_t *_timer = nullptr;

    // Snake body as grid coords; index 0 is the head.
    int _sx[MAX_LEN] = {0};
    int _sy[MAX_LEN] = {0};
    int _len = START_LEN;

    int _food_x = 0;
    int _food_y = 0;

    Dir _dir = Dir::Right;          // current heading
    Dir _next_dir = Dir::Right;     // applied at the next step

    // Latest touch point (screen px); valid once the player has touched.
    bool _have_touch = false;
    int _touch_x = 0;
    int _touch_y = 0;

    // Step pacing.
    uint32_t _last_step_us = 0;
    uint32_t _step_ms = 140;

    Phase _phase = Phase::Playing;
    int _score = 0;
};

} // namespace esp_brookesia::apps
