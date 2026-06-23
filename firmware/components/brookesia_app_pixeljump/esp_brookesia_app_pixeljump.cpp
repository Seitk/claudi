/*
 * Pixel Jump game — see header. Round 466x466 AMOLED, one-button runner.
 *
 * Flow: a single endless run. The blocky hero runs in place on the ground; green
 * cactus blocks scroll in from the right at a speed that ramps up slowly with the
 * score. A falling edge on the BOOT button (GPIO0, active-low) — or a tap anywhere
 * on the screen — launches a parabolic jump when the hero is grounded; presses are
 * ignored mid-air. Clearing an obstacle scores +1; an AABB overlap of the hero and
 * any obstacle ends the run. Game Over shows the score, the best-ever score (kept
 * in a file-scope static across launches), and "tap to retry"; a press restarts.
 * Tick is ~16ms. All gameplay sits in the mid band with the ground at y~320 so
 * nothing clips the round bezel.
 */
#include "esp_brookesia_app_pixeljump.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:pixeljump"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"
#include "game_icons.h"
#include "driver/gpio.h"

#include <cmath>
#include <cstring>

#define APP_NAME "Pixel Jump"

// --- Geometry (screen is 466x466, center 233,233, visible radius ~233) ---
#define SCREEN_CX  233
#define SCREEN_CY  233

#define GROUND_Y   320          // top edge of the ground bar (y, screen px)
#define GROUND_X   28           // ground bar left edge
#define GROUND_W   410          // ground bar width (well inside the bezel)
#define GROUND_H   8            // ground bar thickness

#define HERO_X     120          // hero left edge (fixed; world scrolls past it)
#define HERO_W     30           // hero overall width (body block)
#define HERO_BODY_H 30          // body block height
#define HERO_HEAD_H 18          // head block height
#define HERO_HEAD_W 22          // head block width (narrower than body)
#define HERO_LEG_H  12          // leg block height
#define HERO_LEG_W  10          // each leg block width
#define HERO_TOTAL_H (HERO_HEAD_H + HERO_BODY_H + HERO_LEG_H)  // 60

// Obstacle sizing.
#define OBS_W       22          // cactus block width
#define OBS_H_MIN   34          // shortest cactus
#define OBS_H_MAX   62          // tallest cactus
#define SPAWN_X     470.0f      // just off the right edge

// --- Jump / scroll physics (per 16ms tick) ---
#define JUMP_V      11.0f       // initial upward speed (px/tick)
#define GRAVITY     0.65f       // downward accel (px/tick^2)
#define SPEED_START 4.5f        // initial scroll speed
#define SPEED_MAX   11.0f       // capped scroll speed
#define SPEED_GAIN  0.18f       // speed added per point scored

// Gap (in px) between obstacles, randomized within a band that tightens slightly.
#define GAP_MIN     200
#define GAP_RAND    220

#define COL_BG     0x07070C
#define COL_GROUND 0x6A6A78     // gray ground
#define COL_HERO   0xFFD93B     // bright yellow hero
#define COL_HERO_HEAD 0xFFE873  // slightly lighter head
#define COL_OBS    0x4CD964     // green cactus
#define COL_TEXT   0xFFFFFF
#define COL_SUB    0x9DE0FF     // soft blue subtitle

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

PixelJumpApp *PixelJumpApp::_instance = nullptr;

// Best score persists across launches (the only allowed cross-launch state).
static int s_best_score = 0;

static float frand(void) { return (float)esp_random() / 4294967296.0f; }

// Helper: style an object as a crisp square retro block of one solid color.
static void make_block(lv_obj_t *o, uint32_t color)
{
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

PixelJumpApp *PixelJumpApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PixelJumpApp();
    }
    return _instance;
}

PixelJumpApp::PixelJumpApp(): App(APP_NAME, &icon_pixeljump, true, false, false) {}
PixelJumpApp::~PixelJumpApp() {}

bool PixelJumpApp::run(void)
{
    ESP_UTILS_LOGI("pixeljump run");

    // Configure the BOOT button on GPIO0 as a pulled-up input. Idempotent: the
    // claudi app may already drive GPIO0 the same way, so re-applying is safe.
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << 0;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // The whole screen is a press target: a tap jumps during play and retries on
    // the game-over screen (a friendly fallback to the button). The back gesture
    // still works — Brookesia reads the touch device directly.
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, onScreenPress, LV_EVENT_PRESSED, this);

    // Long thin gray ground bar.
    _ground = lv_obj_create(scr);
    make_block(_ground, COL_GROUND);
    lv_obj_set_size(_ground, GROUND_W, GROUND_H);
    lv_obj_set_pos(_ground, GROUND_X, GROUND_Y);

    // Hero — stacked square blocks (head, body, two legs), bright yellow.
    _hero_body = lv_obj_create(scr);
    make_block(_hero_body, COL_HERO);
    lv_obj_set_size(_hero_body, HERO_W, HERO_BODY_H);

    _hero_head = lv_obj_create(scr);
    make_block(_hero_head, COL_HERO_HEAD);
    lv_obj_set_size(_hero_head, HERO_HEAD_W, HERO_HEAD_H);

    _hero_legL = lv_obj_create(scr);
    make_block(_hero_legL, COL_HERO);
    lv_obj_set_size(_hero_legL, HERO_LEG_W, HERO_LEG_H);

    _hero_legR = lv_obj_create(scr);
    make_block(_hero_legR, COL_HERO);
    lv_obj_set_size(_hero_legR, HERO_LEG_W, HERO_LEG_H);

    // Obstacle pool — created once, then recycled by show/hide + reposition.
    for (int i = 0; i < OBSTACLES; ++i) {
        _obs[i].block = lv_obj_create(scr);
        make_block(_obs[i].block, COL_OBS);
        lv_obj_set_size(_obs[i].block, OBS_W, OBS_H_MIN);
        lv_obj_add_flag(_obs[i].block, LV_OBJ_FLAG_HIDDEN);
    }

    // Score, top center.
    _score_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_score_label, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(_score_label, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(_score_label, LV_ALIGN_TOP_MID, 0, 54);
    lv_label_set_text(_score_label, "0");

    // Centered game-over banner (hidden during play).
    _over_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_over_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_over_label, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(_over_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_over_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(_over_label, "");
    lv_obj_add_flag(_over_label, LV_OBJ_FLAG_HIDDEN);

    resetGame();

    _timer = lv_timer_create(onTick, 16, this);
    return true;
}

bool PixelJumpApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void PixelJumpApp::resetGame(void)
{
    _phase = Phase::Playing;
    _score = 0;
    _hero_y = 0.0f;
    _hero_vy = 0.0f;
    _airborne = false;
    _speed = SPEED_START;
    _prev_level = 1;   // assume released (pulled high) at start
    _leg_phase = 0;

    lv_label_set_text(_score_label, "0");
    lv_obj_clear_flag(_score_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_over_label, LV_OBJ_FLAG_HIDDEN);

    // Hero visible and grounded.
    lv_obj_clear_flag(_hero_head, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_hero_body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_hero_legL, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_hero_legR, LV_OBJ_FLAG_HIDDEN);
    layoutHero();

    // Stagger the obstacle pool off-screen so they file in with gaps.
    float x = SPAWN_X;
    for (int i = 0; i < OBSTACLES; ++i) {
        spawnObstacle(i);
        _obs[i].x = x;
        lv_obj_set_pos(_obs[i].block, (int)x, GROUND_Y - _obs[i].h);
        x += GAP_MIN + (float)(esp_random() % GAP_RAND);
    }
}

// Position all four hero blocks for the current jump height (_hero_y above ground).
void PixelJumpApp::layoutHero(void)
{
    // Feet sit on the ground line, lifted by _hero_y while airborne.
    int feet_y = GROUND_Y - (int)_hero_y;
    int legs_y = feet_y - HERO_LEG_H;
    int body_y = legs_y - HERO_BODY_H;
    int head_y = body_y - HERO_HEAD_H;

    lv_obj_set_pos(_hero_body, HERO_X, body_y);
    // Head centered over the body.
    lv_obj_set_pos(_hero_head, HERO_X + (HERO_W - HERO_HEAD_W) / 2, head_y);

    // Two legs. While running (grounded) they alternate a small bob for life;
    // while airborne they tuck together.
    int legL_y = legs_y;
    int legR_y = legs_y;
    if (!_airborne) {
        // _leg_phase toggles which leg is raised, giving a simple run cycle.
        if ((_leg_phase / 4) & 1) {
            legL_y -= 4;
        } else {
            legR_y -= 4;
        }
    }
    lv_obj_set_pos(_hero_legL, HERO_X + 2, legL_y);
    lv_obj_set_pos(_hero_legR, HERO_X + HERO_W - HERO_LEG_W - 2, legR_y);
}

// (Re)assign obstacle idx a fresh random height and mark it ready to scroll.
void PixelJumpApp::spawnObstacle(int idx)
{
    Obstacle &o = _obs[idx];
    o.w = OBS_W;
    o.h = OBS_H_MIN + (int)(frand() * (OBS_H_MAX - OBS_H_MIN));
    o.active = true;
    o.passed = false;
    lv_obj_set_size(o.block, o.w, o.h);
    lv_obj_clear_flag(o.block, LV_OBJ_FLAG_HIDDEN);
}

// Launch a parabolic hop, but only when grounded (ignore mid-air presses).
void PixelJumpApp::jump(void)
{
    if (_airborne) {
        return;
    }
    _airborne = true;
    _hero_vy = JUMP_V;
}

void PixelJumpApp::enterGameOver(void)
{
    _phase = Phase::GameOver;
    if (_score > s_best_score) {
        s_best_score = _score;
    }
    lv_obj_add_flag(_score_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(_over_label, "Score %d\nBest %d\n\ntap to retry",
                          _score, s_best_score);
    lv_obj_clear_flag(_over_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_over_label);
}

void PixelJumpApp::onScreenPress(lv_event_t *e)
{
    PixelJumpApp *self = static_cast<PixelJumpApp *>(lv_event_get_user_data(e));
    if (self) {
        self->handleScreenPress();
    }
}

void PixelJumpApp::handleScreenPress(void)
{
    // Touch fallback for the button: jump during play, retry when over.
    if (_phase == Phase::Playing) {
        jump();
    } else {
        resetGame();
    }
}

void PixelJumpApp::onTick(lv_timer_t *timer)
{
    PixelJumpApp *self = static_cast<PixelJumpApp *>(lv_timer_get_user_data(timer));
    if (self) {
        self->tick();
    }
}

void PixelJumpApp::tick(void)
{
    // Poll the BOOT button every tick (active-low). A falling edge is one press;
    // edge-detection at the tick rate naturally debounces it.
    int level = gpio_get_level((gpio_num_t)0);
    bool button_edge = (_prev_level == 1 && level == 0);
    _prev_level = level;

    if (_phase != Phase::Playing) {
        if (button_edge) {
            resetGame();
        }
        return;
    }

    if (button_edge) {
        jump();
    }

    // Vertical motion: integrate the jump arc, land back on the ground.
    if (_airborne) {
        _hero_y += _hero_vy;
        _hero_vy -= GRAVITY;
        if (_hero_y <= 0.0f) {
            _hero_y = 0.0f;
            _hero_vy = 0.0f;
            _airborne = false;
        }
    } else {
        _leg_phase++;  // advance the simple running-leg cycle
    }
    layoutHero();

    // Hero AABB (using the body+head span as the collision box).
    int hero_left = HERO_X;
    int hero_right = HERO_X + HERO_W;
    int hero_bottom = GROUND_Y - (int)_hero_y;          // feet
    int hero_top = hero_bottom - (HERO_BODY_H + HERO_HEAD_H + HERO_LEG_H);

    // Scroll obstacles leftward, score on pass, detect collisions, recycle.
    for (int i = 0; i < OBSTACLES; ++i) {
        Obstacle &o = _obs[i];
        if (!o.active) {
            continue;
        }
        o.x -= _speed;

        int obs_left = (int)o.x;
        int obs_right = obs_left + o.w;
        int obs_top = GROUND_Y - o.h;
        int obs_bottom = GROUND_Y;
        lv_obj_set_pos(o.block, obs_left, obs_top);

        // Score once the hero has fully cleared this obstacle horizontally.
        if (!o.passed && obs_right < hero_left) {
            o.passed = true;
            _score += 1;
            lv_label_set_text_fmt(_score_label, "%d", _score);
            // Speed ramps up slowly with the score, up to a cap.
            _speed = SPEED_START + (float)_score * SPEED_GAIN;
            if (_speed > SPEED_MAX) {
                _speed = SPEED_MAX;
            }
        }

        // AABB overlap of hero vs obstacle -> game over.
        bool overlap = (hero_left < obs_right) && (hero_right > obs_left) &&
                       (hero_top < obs_bottom) && (hero_bottom > obs_top);
        if (overlap) {
            enterGameOver();
            return;
        }

        // Recycle once fully off the left edge: re-home behind the rightmost
        // obstacle with a fresh random height and gap.
        if (obs_right < -10) {
            float rightmost = SPAWN_X;
            for (int j = 0; j < OBSTACLES; ++j) {
                if (j != i && _obs[j].active && _obs[j].x > rightmost) {
                    rightmost = _obs[j].x;
                }
            }
            spawnObstacle(i);
            o.x = rightmost + GAP_MIN + (float)(esp_random() % GAP_RAND);
            lv_obj_set_pos(o.block, (int)o.x, GROUND_Y - o.h);
        }
    }
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PixelJumpApp, APP_NAME, []()
{
    return std::shared_ptr<PixelJumpApp>(PixelJumpApp::requestInstance(), [](PixelJumpApp *) {});
})

} // namespace esp_brookesia::apps
