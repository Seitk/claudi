/*
 * Sky Hop — see header. Round 466x466 AMOLED, tap-to-flap.
 *
 * Flow: Ready (tap to start) -> Playing -> GameOver (tap to replay). Gravity
 * constantly pulls the plane down; each tap gives it an upward kick. Walls scroll
 * right-to-left with a gap to fly through; clearing one scores, touching a wall or
 * the ground ends the run. Walls speed up and crowd in as the score climbs.
 */
#include "esp_brookesia_app_voiceplane.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:skyhop"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"
#include "game_icons.h"

#include <cmath>

#define APP_NAME "Sky Hop"

#define SCREEN_W   466
#define SCREEN_H   466
#define SCREEN_CX  233
#define SCREEN_CY  233

#define PLANE_X     140.0f   // fixed horizontal position
#define PLANE_R      16.0f   // collision radius
#define GRAVITY    1150.0f   // px/s^2 down
#define FLAP_VY    -360.0f   // upward kick applied on each tap
#define VY_MAX      520.0f   // terminal fall speed

#define WALL_W       50
#define WALL_GAP    175.0f   // vertical opening
#define WALL_SPACING 250.0f  // horizontal distance between walls

#define COL_BG     0x0B1020
#define COL_PLANE  0xFFC400
#define COL_WALL   0x4CD964
#define COL_BANNER 0xE8533F

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

VoicePlaneApp *VoicePlaneApp::_instance = nullptr;

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }
static float frand(void) { return (float)esp_random() / 4294967296.0f; }

VoicePlaneApp *VoicePlaneApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new VoicePlaneApp();
    }
    return _instance;
}

VoicePlaneApp::VoicePlaneApp(): App(APP_NAME, &icon_voiceplane, true, false, false) {}
VoicePlaneApp::~VoicePlaneApp() {}

void VoicePlaneApp::layoutPlane(void)
{
    lv_obj_align(_plane, LV_ALIGN_CENTER, (int)PLANE_X - SCREEN_CX, (int)_plane_y - SCREEN_CY);
}

void VoicePlaneApp::layoutWall(Wall &w)
{
    int gap_top = (int)(w.gap_cy - WALL_GAP / 2.0f);
    int gap_bot = (int)(w.gap_cy + WALL_GAP / 2.0f);
    if (gap_top < 0) gap_top = 0;
    if (gap_bot > SCREEN_H) gap_bot = SCREEN_H;

    int top_h = gap_top;
    if (top_h < 1) top_h = 1;
    lv_obj_set_size(w.top, WALL_W, top_h);
    lv_obj_align(w.top, LV_ALIGN_TOP_LEFT, (int)w.x, 0);

    int bot_h = SCREEN_H - gap_bot;
    if (bot_h < 1) bot_h = 1;
    lv_obj_set_size(w.bottom, WALL_W, bot_h);
    lv_obj_align(w.bottom, LV_ALIGN_TOP_LEFT, (int)w.x, gap_bot);
}

bool VoicePlaneApp::run(void)
{
    ESP_UTILS_LOGI("skyhop run");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, onPressed, LV_EVENT_PRESSED, this);

    // Wall pool (two bars each), created once.
    for (int i = 0; i < kMaxWalls; ++i) {
        Wall &w = _walls[i];
        for (int k = 0; k < 2; ++k) {
            lv_obj_t *bar = lv_obj_create(scr);
            lv_obj_set_style_radius(bar, 8, 0);
            lv_obj_set_style_bg_color(bar, lv_color_hex(COL_WALL), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_set_style_pad_all(bar, 0, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
            if (k == 0) w.top = bar; else w.bottom = bar;
        }
        w.active = false;
    }

    // The plane (a little rounded body with a cockpit dot).
    _plane = lv_obj_create(scr);
    lv_obj_set_size(_plane, 46, 30);
    lv_obj_set_style_radius(_plane, 14, 0);
    lv_obj_set_style_bg_color(_plane, lv_color_hex(COL_PLANE), 0);
    lv_obj_set_style_bg_opa(_plane, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_plane, 0, 0);
    lv_obj_set_style_pad_all(_plane, 0, 0);
    lv_obj_clear_flag(_plane, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_plane, LV_OBJ_FLAG_SCROLLABLE);

    _plane_eye = lv_obj_create(_plane);
    lv_obj_set_size(_plane_eye, 10, 10);
    lv_obj_set_style_radius(_plane_eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(_plane_eye, lv_color_hex(0x1A1320), 0);
    lv_obj_set_style_border_width(_plane_eye, 0, 0);
    lv_obj_set_style_pad_all(_plane_eye, 0, 0);
    lv_obj_clear_flag(_plane_eye, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_plane_eye, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(_plane_eye, LV_ALIGN_RIGHT_MID, -6, 0);

    // Score, top center.
    _score_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_score_label, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(_score_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(_score_label, LV_ALIGN_TOP_MID, 0, 54);
    lv_label_set_text(_score_label, "0");

    // Title / banner + hint.
    _big = lv_label_create(scr);
    lv_obj_set_style_text_font(_big, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(_big, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_big, LV_ALIGN_CENTER, 0, -24);

    _hint = lv_label_create(scr);
    lv_obj_set_style_text_font(_hint, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(_hint, lv_color_hex(0x9AB0D0), 0);
    lv_obj_set_style_text_align(_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_hint, LV_ALIGN_CENTER, 0, 48);

    resetToReady();

    _last_tick_us = now_us();
    _timer = lv_timer_create(onTick, 30, this);
    return true;
}

bool VoicePlaneApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void VoicePlaneApp::resetToReady(void)
{
    _phase = Phase::Ready;
    _plane_y = SCREEN_CY;
    _plane_vy = 0;
    for (int i = 0; i < kMaxWalls; ++i) {
        _walls[i].active = false;
        lv_obj_add_flag(_walls[i].top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_walls[i].bottom, LV_OBJ_FLAG_HIDDEN);
    }
    layoutPlane();
    lv_label_set_text(_big, "Sky\nHop");
    lv_obj_set_style_text_color(_big, lv_color_hex(COL_PLANE), 0);
    lv_label_set_text(_hint, "tap to fly\ntap to start");
    lv_obj_clear_flag(_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_hint, LV_OBJ_FLAG_HIDDEN);
}

void VoicePlaneApp::startPlaying(void)
{
    _score = 0;
    _plane_y = SCREEN_CY;
    _plane_vy = FLAP_VY;  // start with a little hop so it doesn't drop instantly
    lv_label_set_text(_score_label, "0");
    for (int i = 0; i < kMaxWalls; ++i) {
        _walls[i].active = false;
        lv_obj_add_flag(_walls[i].top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_walls[i].bottom, LV_OBJ_FLAG_HIDDEN);
    }
    layoutPlane();
    lv_obj_add_flag(_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_hint, LV_OBJ_FLAG_HIDDEN);
    _phase = Phase::Playing;
    _next_spawn_us = now_us() + 500000;  // brief beat before the first wall
}

void VoicePlaneApp::enterGameOver(void)
{
    _phase = Phase::GameOver;
    lv_label_set_text(_big, "CRASH!");
    lv_obj_set_style_text_color(_big, lv_color_hex(COL_BANNER), 0);
    lv_label_set_text_fmt(_hint, "score %d\ntap to play again", _score);
    lv_obj_clear_flag(_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_hint, LV_OBJ_FLAG_HIDDEN);
}

void VoicePlaneApp::spawnWall(void)
{
    int slot = -1;
    for (int i = 0; i < kMaxWalls; ++i) {
        if (!_walls[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        return;
    }
    Wall &w = _walls[slot];
    w.x = SCREEN_W + 4;
    float margin = WALL_GAP / 2.0f + 24.0f;
    w.gap_cy = margin + frand() * (SCREEN_H - 2.0f * margin);
    w.passed = false;
    w.active = true;
    layoutWall(w);
    lv_obj_clear_flag(w.top, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(w.bottom, LV_OBJ_FLAG_HIDDEN);
}

void VoicePlaneApp::onPressed(lv_event_t *e)
{
    VoicePlaneApp *self = static_cast<VoicePlaneApp *>(lv_event_get_user_data(e));
    if (self) self->handlePressed();
}

void VoicePlaneApp::handlePressed(void)
{
    switch (_phase) {
    case Phase::Playing:
        _plane_vy = FLAP_VY;  // flap!
        break;
    case Phase::Ready:
    case Phase::GameOver:
        startPlaying();
        break;
    }
}

void VoicePlaneApp::onTick(lv_timer_t *timer)
{
    VoicePlaneApp *self = static_cast<VoicePlaneApp *>(lv_timer_get_user_data(timer));
    if (self) self->tick();
}

void VoicePlaneApp::tick(void)
{
    uint32_t now = now_us();
    float dt = (float)(now - _last_tick_us) / 1000000.0f;
    _last_tick_us = now;
    if (dt > 0.1f) dt = 0.1f;

    if (_phase != Phase::Playing) {
        return;
    }

    // Plane physics: gravity down, taps kick it up (handled in handlePressed).
    _plane_vy += GRAVITY * dt;
    if (_plane_vy > VY_MAX) _plane_vy = VY_MAX;
    if (_plane_vy < -VY_MAX) _plane_vy = -VY_MAX;
    _plane_y += _plane_vy * dt;

    // Hit the ground -> crash. Soft ceiling at the top (clamp, don't kill).
    if (_plane_y < PLANE_R) {
        _plane_y = PLANE_R;
        if (_plane_vy < 0) _plane_vy = 0;
    }
    if (_plane_y > SCREEN_H - PLANE_R) {
        _plane_y = SCREEN_H - PLANE_R;
        layoutPlane();
        enterGameOver();
        return;
    }
    layoutPlane();

    // Spawn walls on a cadence that tightens with score.
    float speed = 130.0f + _score * 5.0f;
    if (speed > 300.0f) speed = 300.0f;
    if (now >= _next_spawn_us) {
        spawnWall();
        uint32_t interval_us = (uint32_t)(WALL_SPACING / speed * 1000000.0f);
        _next_spawn_us = now + interval_us;
    }

    // Scroll walls, score, and collide.
    for (int i = 0; i < kMaxWalls; ++i) {
        Wall &w = _walls[i];
        if (!w.active) {
            continue;
        }
        w.x -= speed * dt;

        if (!w.passed && (w.x + WALL_W) < PLANE_X) {
            w.passed = true;
            _score++;
            lv_label_set_text_fmt(_score_label, "%d", _score);
        }

        bool x_overlap = (PLANE_X + PLANE_R > w.x) && (PLANE_X - PLANE_R < w.x + WALL_W);
        if (x_overlap) {
            float gap_top = w.gap_cy - WALL_GAP / 2.0f;
            float gap_bot = w.gap_cy + WALL_GAP / 2.0f;
            if (_plane_y - PLANE_R < gap_top || _plane_y + PLANE_R > gap_bot) {
                enterGameOver();
                return;
            }
        }

        if (w.x + WALL_W < -4) {
            w.active = false;
            lv_obj_add_flag(w.top, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(w.bottom, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        layoutWall(w);
    }
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, VoicePlaneApp, APP_NAME, []()
{
    return std::shared_ptr<VoicePlaneApp>(VoicePlaneApp::requestInstance(), [](VoicePlaneApp *) {});
})

} // namespace esp_brookesia::apps
