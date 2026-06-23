/*
 * Tilt Maze — a relaxed roll-the-ball-to-the-goal maze, shipped as an
 * ESP-Brookesia phone app (self-registers into the launcher).
 *
 * Tilt the board and a bright pixel ball rolls through a blocky maze under
 * simple accelerometer-driven physics: tilt adds velocity, friction bleeds it
 * off, and the ball is swept (AABB vs wall-cell AABBs) so it never tunnels
 * through walls. Reach the green goal block and a brief "Level done!" flash
 * plays before the next hand-designed level loads (the set wraps around). It's
 * a calm game with no fail state — just keep tilting to the goal.
 *
 * If the on-board IMU is missing, the game shows a "Tilt sensor unavailable"
 * message and parks the ball; the back gesture still returns to the launcher.
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class TiltMazeApp: public systems::phone::App {
public:
    static TiltMazeApp *requestInstance();
    ~TiltMazeApp();

protected:
    TiltMazeApp();

    // Build the UI on the default screen (lv_scr_act()).
    bool run(void) override;
    // Back gesture: leave the game, return to launcher.
    bool back(void) override;

private:
    static TiltMazeApp *_instance;

    // Pool of recycled wall blocks. The maze is 10x10 (GRID/CELL live as
    // file-scope macros in the .cpp so the LEVELS table can use them); a full
    // 10x10 grid never needs more than 100 wall blocks. Size generously.
    static constexpr int MAX_WALLS = 100;

    // ~16ms physics loop: read tilt, integrate, resolve wall collisions.
    static void onTick(lv_timer_t *timer);
    void tick(void);

    void loadLevel(int index);  // (re)build walls + place ball/goal for a level
    void resetGame(void);       // start at level 0 (called fresh in run())
    void syncBallObj(void);     // copy float ball position into the LVGL object

    // True if the ball's AABB at (x,y) overlaps any wall cell of the level.
    bool hitsWall(float x, float y) const;

    // Wall block pool: created once in run(), shown/positioned per level.
    lv_obj_t *_walls[MAX_WALLS] = {nullptr};
    int _wall_count = 0;        // how many blocks are in use this level

    // Gameplay widgets (created in run()).
    lv_obj_t *_ball = nullptr;  // bright-yellow square block
    lv_obj_t *_goal = nullptr;  // green square block at the 'G' cell
    lv_obj_t *_hud = nullptr;   // "Level N" top-center
    lv_obj_t *_flash = nullptr; // "Level done!" celebration banner
    lv_obj_t *_nosensor = nullptr; // "Tilt sensor unavailable" message
    lv_timer_t *_timer = nullptr;

    // Ball physics state (pixel coordinates of the ball's top-left corner).
    float _bx = 0.0f, _by = 0.0f;
    float _vx = 0.0f, _vy = 0.0f;

    // Last good accelerometer reading (reused if a read fails this tick).
    float _ax = 0.0f, _ay = 0.0f;

    // Goal cell (grid coords) and the goal block's pixel top-left.
    int _goal_col = 0, _goal_row = 0;

    int _level = 0;             // current level index
    bool _imu_ok = false;       // false -> show fallback, skip physics
    bool _level_done = false;   // in the brief "Level done!" flash
    uint32_t _flash_until_us = 0;
};

} // namespace esp_brookesia::apps
