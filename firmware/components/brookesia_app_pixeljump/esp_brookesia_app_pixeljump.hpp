/*
 * Pixel Jump — an endless one-button runner (Chrome-dino style), shipped as an
 * ESP-Brookesia phone app (self-registers into the launcher).
 *
 * A blocky yellow hero runs in place on a ground line while green "cactus" blocks
 * scroll in from the right and speed up slowly with the score. Press the physical
 * BOOT button (GPIO0) — or tap anywhere on the screen — to launch a parabolic jump
 * over the obstacles. Clear an obstacle for +1; collide (AABB overlap) and it's
 * Game Over, showing the score, the best-ever score, and a "tap to retry" prompt.
 * Everything is drawn from crisp square blocks in a small bright retro palette.
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class PixelJumpApp: public systems::phone::App {
public:
    static PixelJumpApp *requestInstance();
    ~PixelJumpApp();

protected:
    PixelJumpApp();

    // Build the UI on the default screen (lv_scr_act()).
    bool run(void) override;
    // Back gesture: leave the game, return to launcher.
    bool back(void) override;

private:
    static PixelJumpApp *_instance;

    static constexpr int OBSTACLES = 4;  // recycled pool of cactus blocks

    enum class Phase { Playing, GameOver };

    // ~16ms game loop: polls the button, scrolls obstacles, runs the jump arc.
    static void onTick(lv_timer_t *timer);
    void tick(void);

    // Whole-screen press: jump during play, or retry on the game-over screen.
    static void onScreenPress(lv_event_t *e);
    void handleScreenPress(void);

    void resetGame(void);            // (re)initialize a fresh run
    void jump(void);                 // launch a parabolic hop if grounded
    void spawnObstacle(int idx);     // place an obstacle off the right edge
    void layoutHero(void);           // position hero blocks at the current y
    void enterGameOver(void);

    // One obstacle in the recycled pool. Objects are created once in run() and
    // moved/shown/hidden — never created or destroyed per spawn.
    struct Obstacle {
        lv_obj_t *block = nullptr;
        bool active = false;
        bool passed = false;  // already scored when the hero cleared it
        float x = 0.0f;       // left edge, screen px (float for smooth scroll)
        int w = 0;
        int h = 0;
    };
    Obstacle _obs[OBSTACLES];

    // Hero is composed of stacked square blocks (head + body + two legs).
    lv_obj_t *_hero_head = nullptr;
    lv_obj_t *_hero_body = nullptr;
    lv_obj_t *_hero_legL = nullptr;
    lv_obj_t *_hero_legR = nullptr;

    lv_obj_t *_ground = nullptr;       // long thin gray ground bar
    lv_obj_t *_score_label = nullptr;  // score, top center
    lv_obj_t *_over_label = nullptr;   // centered game-over text
    lv_timer_t *_timer = nullptr;

    // Game state.
    Phase _phase = Phase::Playing;
    int _score = 0;
    float _hero_y = 0.0f;     // hero feet offset above the ground (>=0 up)
    float _hero_vy = 0.0f;    // vertical velocity (px/tick), gravity adds in
    bool _airborne = false;
    float _speed = 0.0f;      // current scroll speed (px/tick)
    int _prev_level = 1;      // last BOOT-button level (active-low) for edge detect
    int _leg_phase = 0;       // running-leg animation counter
};

} // namespace esp_brookesia::apps
