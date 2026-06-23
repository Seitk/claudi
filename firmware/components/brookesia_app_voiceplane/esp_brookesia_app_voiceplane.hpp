/*
 * Sky Hop — a tap-to-flap side-scroller for the round AMOLED, shipped as an
 * ESP-Brookesia phone app (self-registers into the launcher).
 *
 * Tap anywhere to give the plane a little lift; gravity pulls it back down.
 * Steer through the gaps in scrolling walls — each one cleared scores a point,
 * and touching a wall or the ground ends the run. The walls come faster as your
 * score climbs.
 *
 * (Originally designed for mic-volume control, but the board's ES7210 mic returns
 * no audio, so control is a screen tap. See git history / memory for that saga.)
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class VoicePlaneApp: public systems::phone::App {
public:
    static VoicePlaneApp *requestInstance();
    ~VoicePlaneApp();

protected:
    VoicePlaneApp();

    bool run(void) override;   // build UI on lv_scr_act()
    bool back(void) override;  // back gesture -> launcher

private:
    static VoicePlaneApp *_instance;

    enum class Phase { Ready, Playing, GameOver };

    static constexpr int kMaxWalls = 5;  // scrolling obstacle pool

    struct Wall {
        lv_obj_t *top = nullptr;     // bar above the gap
        lv_obj_t *bottom = nullptr;  // bar below the gap
        bool active = false;
        bool passed = false;
        float x = 0;       // left edge, pixels
        float gap_cy = 0;  // gap center y
    };

    // ~33fps loop: integrate plane physics, scroll walls, collisions, scoring.
    static void onTick(lv_timer_t *timer);
    void tick(void);

    static void onPressed(lv_event_t *e);   // tap to start / flap / restart
    void handlePressed(void);

    void resetToReady(void);
    void startPlaying(void);
    void enterGameOver(void);
    void spawnWall(void);
    void layoutWall(Wall &w);
    void layoutPlane(void);

    // Widgets (created once in run(); recycled by the core on app close).
    Wall _walls[kMaxWalls];
    lv_obj_t *_plane = nullptr;
    lv_obj_t *_plane_eye = nullptr;     // little cockpit dot
    lv_obj_t *_score_label = nullptr;
    lv_obj_t *_big = nullptr;           // title / GAME OVER
    lv_obj_t *_hint = nullptr;          // "tap to fly" / "tap to play again"
    lv_timer_t *_timer = nullptr;

    // Game state.
    Phase _phase = Phase::Ready;
    int _score = 0;
    float _plane_y = 233;
    float _plane_vy = 0;
    uint32_t _next_spawn_us = 0;
    uint32_t _last_tick_us = 0;
};

} // namespace esp_brookesia::apps
