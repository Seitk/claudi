/*
 * Fruit Ninja — a swipe-to-slice arcade game for the round AMOLED, shipped as an
 * ESP-Brookesia phone app (self-registers into the launcher).
 *
 * Fruit is tossed up from the bottom and arcs under gravity. Drag a finger across
 * the screen to swipe a blade through them: each fruit sliced scores a point and
 * bursts. Let three fruits fall past unsliced and it's game over — and don't slice
 * the bombs. The longer you last, the faster the fruit flies.
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class FruitNinjaApp: public systems::phone::App {
public:
    static FruitNinjaApp *requestInstance();
    ~FruitNinjaApp();

protected:
    FruitNinjaApp();

    bool run(void) override;   // build UI on lv_scr_act()
    bool back(void) override;  // back gesture -> launcher

private:
    static FruitNinjaApp *_instance;

    enum class Phase { Ready, Playing, GameOver };

    static constexpr int kMaxFruits = 10;  // physics object pool
    static constexpr int kMaxSplash = 10;  // slice-burst effect pool
    static constexpr int kTrailLen  = 8;   // blade trail points

    struct Fruit {
        lv_obj_t *obj = nullptr;   // the circle (reused across spawns)
        lv_obj_t *mark = nullptr;  // child glyph: bomb "x" (hidden for fruit)
        bool active = false;
        bool is_bomb = false;
        bool falling = false;      // past apex (for miss detection)
        float x = 0, y = 0;        // center, pixels
        float vx = 0, vy = 0;      // px/s
        float radius = 38;
        uint32_t color = 0xFFFFFF;  // body colour (for the slice burst)
    };
    struct Splash {
        lv_obj_t *obj = nullptr;
        bool active = false;
        uint32_t start_us = 0;
        uint32_t dur_us = 0;
        float x = 0, y = 0;
        float r0 = 0;
    };

    // ~30fps loop: physics, spawning, miss/lives, blade fade, splash animation.
    static void onTick(lv_timer_t *timer);
    void tick(void);

    // Pointer-driven swipe handling.
    static void onPressed(lv_event_t *e);
    static void onPressing(lv_event_t *e);
    static void onReleased(lv_event_t *e);
    void handlePressed(void);
    void handlePressing(void);
    void handleReleased(void);

    void resetToReady(void);
    void startPlaying(void);
    void enterGameOver(void);

    void spawnFruit(void);
    void sliceCheck(float ax, float ay, float bx, float by);  // segment vs fruits
    void burst(float x, float y, uint32_t color, float r);    // spawn a splash
    void layoutFruit(Fruit &f);
    void pushTrail(float x, float y);
    void updateBlade(void);
    void loseLife(void);

    // Widgets (created once in run(); recycled by the core on app close).
    Fruit _fruits[kMaxFruits];
    Splash _splash[kMaxSplash];
    lv_obj_t *_blade = nullptr;       // lv_line following the finger
    lv_obj_t *_score_label = nullptr;
    lv_obj_t *_life_pip[3] = {nullptr, nullptr, nullptr};
    lv_obj_t *_big = nullptr;         // title / GAME OVER banner
    lv_obj_t *_hint = nullptr;        // "tap to start" / "tap to play again"
    lv_timer_t *_timer = nullptr;

    // Blade trail point ring (persistent; lv_line keeps the pointer).
    lv_point_precise_t _trail[kTrailLen];
    int _trail_n = 0;

    // Game state.
    Phase _phase = Phase::Ready;
    int _score = 0;
    int _lives = 3;
    bool _slicing = false;
    bool _have_prev = false;
    float _prev_x = 0, _prev_y = 0;
    uint32_t _next_spawn_us = 0;
    uint32_t _last_tick_us = 0;
};

} // namespace esp_brookesia::apps
