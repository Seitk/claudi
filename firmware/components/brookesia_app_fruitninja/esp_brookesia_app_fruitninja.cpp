/*
 * Fruit Ninja — see header. Round 466x466 AMOLED, swipe to slice.
 *
 * Flow: Ready (tap to start) -> Playing -> GameOver (tap to replay). Fruit and
 * bombs are tossed from the bottom and arc under gravity. A finger drag draws a
 * blade (lv_line trail); the segment between successive touch points is tested
 * against every active fruit. Slicing fruit scores; slicing a bomb ends the run;
 * letting three fruits fall past costs all three lives. Spawn rate and toss speed
 * ramp up with the score.
 */
#include "esp_brookesia_app_fruitninja.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:fruit"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"
#include "game_icons.h"

#include <cmath>
#include <cstring>

#define APP_NAME "Fruit Ninja"

#define SCREEN_CX 233
#define SCREEN_CY 233
#define FLOOR_Y   505    // y past which a falling object is "gone"
#define SPAWN_Y   500    // launch from just below the bottom
#define GRAVITY   900.0f // px/s^2
#define FRUIT_R    38.0f
#define BLADE_HIT  8.0f  // extra slice tolerance around the blade

#define COL_BG     0x07070C
#define COL_BOMB   0x17181F
#define COL_BOMB_BORDER 0xE8533F
#define COL_BLADE  0xEAF6FF
#define COL_PIP_ON  0xE8533F
#define COL_PIP_OFF 0x2A2A33

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

FruitNinjaApp *FruitNinjaApp::_instance = nullptr;

// Cheerful fruit body colours.
static const uint32_t kFruitColors[] = {
    0xFF5252, 0xFF9F1C, 0xFFE03B, 0x4CD964, 0xFF6FB5, 0x9B6DFF,
};
static const int kFruitColorCount = sizeof(kFruitColors) / sizeof(kFruitColors[0]);

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }
static float frand(void) { return (float)esp_random() / 4294967296.0f; }
static float frange(float lo, float hi) { return lo + (hi - lo) * frand(); }

// Distance from point (px,py) to segment (ax,ay)-(bx,by).
static float dist_pt_seg(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    float t = (len2 > 0.0f) ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float cx = ax + t * dx, cy = ay + t * dy;
    float ex = px - cx, ey = py - cy;
    return sqrtf(ex * ex + ey * ey);
}

FruitNinjaApp *FruitNinjaApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new FruitNinjaApp();
    }
    return _instance;
}

FruitNinjaApp::FruitNinjaApp(): App(APP_NAME, &icon_fruitninja, true, false, false) {}
FruitNinjaApp::~FruitNinjaApp() {}

void FruitNinjaApp::layoutFruit(Fruit &f)
{
    int d = (int)(f.radius * 2.0f);
    lv_obj_set_size(f.obj, d, d);
    lv_obj_align(f.obj, LV_ALIGN_CENTER, (int)f.x - SCREEN_CX, (int)f.y - SCREEN_CY);
}

bool FruitNinjaApp::run(void)
{
    ESP_UTILS_LOGI("fruitninja run");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, onPressed, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(scr, onPressing, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(scr, onReleased, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(scr, onReleased, LV_EVENT_PRESS_LOST, this);

    // Fruit pool (created once; shown/hidden as they spawn/despawn).
    for (int i = 0; i < kMaxFruits; ++i) {
        Fruit &f = _fruits[i];
        f.obj = lv_obj_create(scr);
        lv_obj_set_style_radius(f.obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(f.obj, 0, 0);
        lv_obj_set_style_pad_all(f.obj, 0, 0);
        lv_obj_clear_flag(f.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(f.obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(f.obj, LV_OBJ_FLAG_HIDDEN);
        f.mark = lv_label_create(f.obj);
        lv_label_set_text(f.mark, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_font(f.mark, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(f.mark, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(f.mark);
        lv_obj_add_flag(f.mark, LV_OBJ_FLAG_HIDDEN);
        f.active = false;
    }

    // Slice-burst pool.
    for (int i = 0; i < kMaxSplash; ++i) {
        Splash &s = _splash[i];
        s.obj = lv_obj_create(scr);
        lv_obj_set_style_radius(s.obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s.obj, 0, 0);
        lv_obj_set_style_pad_all(s.obj, 0, 0);
        lv_obj_clear_flag(s.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s.obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s.obj, LV_OBJ_FLAG_HIDDEN);
        s.active = false;
    }

    // Blade trail (lv_line). Kept above the fruit.
    _blade = lv_line_create(scr);
    lv_obj_set_style_line_width(_blade, 7, 0);
    lv_obj_set_style_line_color(_blade, lv_color_hex(COL_BLADE), 0);
    lv_obj_set_style_line_rounded(_blade, true, 0);
    lv_obj_clear_flag(_blade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(_blade, LV_OBJ_FLAG_HIDDEN);

    // Score, top center.
    _score_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_score_label, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(_score_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(_score_label, LV_ALIGN_TOP_MID, 0, 54);
    lv_label_set_text(_score_label, "0");

    // Life pips below the score.
    for (int i = 0; i < 3; ++i) {
        _life_pip[i] = lv_obj_create(scr);
        lv_obj_set_size(_life_pip[i], 18, 18);
        lv_obj_set_style_radius(_life_pip[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(_life_pip[i], 0, 0);
        lv_obj_set_style_pad_all(_life_pip[i], 0, 0);
        lv_obj_set_style_bg_color(_life_pip[i], lv_color_hex(COL_PIP_ON), 0);
        lv_obj_clear_flag(_life_pip[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(_life_pip[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(_life_pip[i], LV_ALIGN_TOP_MID, (i - 1) * 28, 108);
    }

    // Title / banner + hint line.
    _big = lv_label_create(scr);
    lv_obj_set_style_text_font(_big, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(_big, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_big, LV_ALIGN_CENTER, 0, -20);

    _hint = lv_label_create(scr);
    lv_obj_set_style_text_font(_hint, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(_hint, lv_color_hex(0x9A9AB0), 0);
    lv_obj_set_style_text_align(_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_hint, LV_ALIGN_CENTER, 0, 44);

    resetToReady();

    _last_tick_us = now_us();
    _timer = lv_timer_create(onTick, 30, this);
    return true;
}

bool FruitNinjaApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void FruitNinjaApp::resetToReady(void)
{
    _phase = Phase::Ready;
    _slicing = false;
    _have_prev = false;
    _trail_n = 0;
    updateBlade();

    for (int i = 0; i < kMaxFruits; ++i) {
        _fruits[i].active = false;
        lv_obj_add_flag(_fruits[i].obj, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < kMaxSplash; ++i) {
        _splash[i].active = false;
        lv_obj_add_flag(_splash[i].obj, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(_big, "Fruit\nNinja");
    lv_obj_set_style_text_color(_big, lv_color_hex(0x4CD964), 0);
    lv_label_set_text(_hint, "swipe to slice\ntap to start");
    lv_obj_clear_flag(_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_hint, LV_OBJ_FLAG_HIDDEN);
}

void FruitNinjaApp::startPlaying(void)
{
    _score = 0;
    _lives = 3;
    _slicing = false;
    _have_prev = false;
    _trail_n = 0;
    updateBlade();
    lv_label_set_text(_score_label, "0");
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_style_bg_color(_life_pip[i], lv_color_hex(COL_PIP_ON), 0);
    }
    for (int i = 0; i < kMaxFruits; ++i) {
        _fruits[i].active = false;
        lv_obj_add_flag(_fruits[i].obj, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_hint, LV_OBJ_FLAG_HIDDEN);
    _phase = Phase::Playing;
    _next_spawn_us = now_us() + 400000;  // a short beat before the first toss
}

void FruitNinjaApp::enterGameOver(void)
{
    _phase = Phase::GameOver;
    _slicing = false;
    _have_prev = false;
    _trail_n = 0;
    updateBlade();
    for (int i = 0; i < kMaxFruits; ++i) {
        _fruits[i].active = false;
        lv_obj_add_flag(_fruits[i].obj, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(_big, "GAME\nOVER");
    lv_obj_set_style_text_color(_big, lv_color_hex(COL_PIP_ON), 0);
    lv_label_set_text_fmt(_hint, "score %d\ntap to play again", _score);
    lv_obj_clear_flag(_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_hint, LV_OBJ_FLAG_HIDDEN);
}

void FruitNinjaApp::loseLife(void)
{
    if (_lives > 0) {
        _lives--;
        lv_obj_set_style_bg_color(_life_pip[_lives], lv_color_hex(COL_PIP_OFF), 0);
    }
    if (_lives <= 0) {
        enterGameOver();
    }
}

void FruitNinjaApp::spawnFruit(void)
{
    int slot = -1;
    for (int i = 0; i < kMaxFruits; ++i) {
        if (!_fruits[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        return;  // pool full; skip this toss
    }
    Fruit &f = _fruits[slot];

    f.radius = FRUIT_R;
    f.x = frange(90.0f, 376.0f);
    f.y = SPAWN_Y;
    // Aim the arc back toward the middle so fruit stays on the round glass.
    float toward_center = (SCREEN_CX - f.x) * 0.45f;
    f.vx = toward_center + frange(-60.0f, 60.0f);
    f.vy = -frange(820.0f, 1000.0f);  // upward
    f.falling = false;

    // Bombs join in once the player has a few points; ~14% of tosses.
    f.is_bomb = (_score >= 4) && (frand() < 0.14f);
    if (f.is_bomb) {
        f.color = COL_BOMB_BORDER;
        lv_obj_set_style_bg_color(f.obj, lv_color_hex(COL_BOMB), 0);
        lv_obj_set_style_bg_opa(f.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(f.obj, lv_color_hex(COL_BOMB_BORDER), 0);
        lv_obj_set_style_border_width(f.obj, 4, 0);
        lv_obj_clear_flag(f.mark, LV_OBJ_FLAG_HIDDEN);
    } else {
        f.color = kFruitColors[esp_random() % kFruitColorCount];
        lv_obj_set_style_bg_color(f.obj, lv_color_hex(f.color), 0);
        lv_obj_set_style_bg_opa(f.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(f.obj, 0, 0);
        lv_obj_add_flag(f.mark, LV_OBJ_FLAG_HIDDEN);
    }

    f.active = true;
    layoutFruit(f);
    lv_obj_clear_flag(f.obj, LV_OBJ_FLAG_HIDDEN);
}

void FruitNinjaApp::burst(float x, float y, uint32_t color, float r)
{
    int slot = -1;
    for (int i = 0; i < kMaxSplash; ++i) {
        if (!_splash[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        return;
    }
    Splash &s = _splash[slot];
    s.x = x; s.y = y; s.r0 = r;
    s.start_us = now_us();
    s.dur_us = 260000;
    s.active = true;
    lv_obj_set_style_bg_color(s.obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(s.obj, LV_OPA_COVER, 0);
    int d = (int)(r * 2.0f);
    lv_obj_set_size(s.obj, d, d);
    lv_obj_align(s.obj, LV_ALIGN_CENTER, (int)x - SCREEN_CX, (int)y - SCREEN_CY);
    lv_obj_clear_flag(s.obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s.obj);
}

void FruitNinjaApp::sliceCheck(float ax, float ay, float bx, float by)
{
    for (int i = 0; i < kMaxFruits; ++i) {
        Fruit &f = _fruits[i];
        if (!f.active) {
            continue;
        }
        if (dist_pt_seg(f.x, f.y, ax, ay, bx, by) <= f.radius + BLADE_HIT) {
            if (f.is_bomb) {
                burst(f.x, f.y, COL_BOMB_BORDER, f.radius * 1.3f);
                f.active = false;
                lv_obj_add_flag(f.obj, LV_OBJ_FLAG_HIDDEN);
                enterGameOver();
                return;
            }
            // Fruit sliced: score + juicy burst in the fruit's colour.
            burst(f.x, f.y, f.color, f.radius);
            f.active = false;
            lv_obj_add_flag(f.obj, LV_OBJ_FLAG_HIDDEN);
            _score++;
            lv_label_set_text_fmt(_score_label, "%d", _score);
        }
    }
}

void FruitNinjaApp::pushTrail(float x, float y)
{
    if (_trail_n < kTrailLen) {
        _trail[_trail_n].x = (lv_value_precise_t)x;
        _trail[_trail_n].y = (lv_value_precise_t)y;
        _trail_n++;
    } else {
        for (int i = 1; i < kTrailLen; ++i) {
            _trail[i - 1] = _trail[i];
        }
        _trail[kTrailLen - 1].x = (lv_value_precise_t)x;
        _trail[kTrailLen - 1].y = (lv_value_precise_t)y;
    }
}

void FruitNinjaApp::updateBlade(void)
{
    if (_trail_n >= 2) {
        lv_line_set_points(_blade, _trail, _trail_n);
        lv_obj_clear_flag(_blade, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(_blade);
    } else {
        lv_obj_add_flag(_blade, LV_OBJ_FLAG_HIDDEN);
    }
}

void FruitNinjaApp::onPressed(lv_event_t *e)
{
    FruitNinjaApp *self = static_cast<FruitNinjaApp *>(lv_event_get_user_data(e));
    if (self) self->handlePressed();
}
void FruitNinjaApp::onPressing(lv_event_t *e)
{
    FruitNinjaApp *self = static_cast<FruitNinjaApp *>(lv_event_get_user_data(e));
    if (self) self->handlePressing();
}
void FruitNinjaApp::onReleased(lv_event_t *e)
{
    FruitNinjaApp *self = static_cast<FruitNinjaApp *>(lv_event_get_user_data(e));
    if (self) self->handleReleased();
}

void FruitNinjaApp::handlePressed(void)
{
    if (_phase != Phase::Playing) {
        startPlaying();
        return;
    }
    lv_indev_t *in = lv_indev_active();
    if (!in) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    _prev_x = (float)p.x;
    _prev_y = (float)p.y;
    _have_prev = true;
    _slicing = true;
    _trail_n = 0;
    pushTrail(_prev_x, _prev_y);
}

void FruitNinjaApp::handlePressing(void)
{
    if (_phase != Phase::Playing || !_slicing) {
        return;
    }
    lv_indev_t *in = lv_indev_active();
    if (!in) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    float cx = (float)p.x, cy = (float)p.y;

    if (_have_prev && (fabsf(cx - _prev_x) + fabsf(cy - _prev_y) >= 2.0f)) {
        sliceCheck(_prev_x, _prev_y, cx, cy);
    }
    pushTrail(cx, cy);
    _prev_x = cx;
    _prev_y = cy;
    _have_prev = true;
    updateBlade();
}

void FruitNinjaApp::handleReleased(void)
{
    _slicing = false;
    _have_prev = false;
    // The trail fades out over the next few ticks (see tick()).
}

void FruitNinjaApp::onTick(lv_timer_t *timer)
{
    FruitNinjaApp *self = static_cast<FruitNinjaApp *>(lv_timer_get_user_data(timer));
    if (self) self->tick();
}

void FruitNinjaApp::tick(void)
{
    uint32_t now = now_us();
    float dt = (float)(now - _last_tick_us) / 1000000.0f;
    _last_tick_us = now;
    if (dt > 0.1f) dt = 0.1f;  // clamp after a stall

    // Fade the blade trail when the finger is up.
    if (!_slicing && _trail_n > 0) {
        for (int i = 1; i < _trail_n; ++i) {
            _trail[i - 1] = _trail[i];
        }
        _trail_n--;
        updateBlade();
    }

    // Animate slice bursts (grow + fade), independent of game phase.
    for (int i = 0; i < kMaxSplash; ++i) {
        Splash &s = _splash[i];
        if (!s.active) {
            continue;
        }
        float t = (float)(now - s.start_us) / (float)s.dur_us;
        if (t >= 1.0f) {
            s.active = false;
            lv_obj_add_flag(s.obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        float r = s.r0 * (1.0f + 1.2f * t);
        int d = (int)(r * 2.0f);
        lv_obj_set_size(s.obj, d, d);
        lv_obj_align(s.obj, LV_ALIGN_CENTER, (int)s.x - SCREEN_CX, (int)s.y - SCREEN_CY);
        lv_obj_set_style_bg_opa(s.obj, (lv_opa_t)(255.0f * (1.0f - t)), 0);
    }

    if (_phase != Phase::Playing) {
        return;
    }

    // Spawn cadence ramps up with score.
    if (now >= _next_spawn_us) {
        spawnFruit();
        // Occasional double toss as it heats up.
        if (_score >= 8 && frand() < 0.35f) {
            spawnFruit();
        }
        int interval_ms = 950 - _score * 9;
        if (interval_ms < 360) interval_ms = 360;
        _next_spawn_us = now + (uint32_t)interval_ms * 1000;
    }

    // Physics + miss detection.
    for (int i = 0; i < kMaxFruits; ++i) {
        Fruit &f = _fruits[i];
        if (!f.active) {
            continue;
        }
        f.vy += GRAVITY * dt;
        f.x += f.vx * dt;
        f.y += f.vy * dt;
        if (f.vy > 0) {
            f.falling = true;
        }
        if (f.falling && f.y > FLOOR_Y) {
            // Off the bottom: a dropped fruit costs a life; a bomb is harmless.
            bool was_bomb = f.is_bomb;
            f.active = false;
            lv_obj_add_flag(f.obj, LV_OBJ_FLAG_HIDDEN);
            if (!was_bomb) {
                loseLife();
                if (_phase != Phase::Playing) {
                    return;  // game ended inside loseLife()
                }
            }
            continue;
        }
        layoutFruit(f);
    }
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, FruitNinjaApp, APP_NAME, []()
{
    return std::shared_ptr<FruitNinjaApp>(FruitNinjaApp::requestInstance(), [](FruitNinjaApp *) {});
})

} // namespace esp_brookesia::apps
