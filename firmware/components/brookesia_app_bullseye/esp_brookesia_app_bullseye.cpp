/*
 * Bullseye game — see header. Round 466x466 AMOLED.
 *
 * Flow: 3-2-1-GO countdown → rings spawn one at a time at random positions and
 * shrink from R_MAX down onto a fixed target ring. A tap is scored by how close
 * the approach ring is to the target radius at that instant. Letting a ring
 * collapse untapped, or tapping far too early, costs a life. Three lives lost =
 * GAME OVER (tap to replay). Each successful hit shrinks future rings a little
 * faster.
 */
#include "esp_brookesia_app_bullseye.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:bullseye"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"
#include "game_icons.h"

#include <cmath>
#include <cstring>

#define APP_NAME "Bullseye"

// --- Geometry (screen is 466x466, center 233,233, visible radius ~233) ---
#define SCREEN_CX 233
#define SCREEN_CY 233
#define R_MAX       84.0f   // approach ring radius at spawn
#define R_TARGET    22.0f   // the bullseye radius — perfect tap matches this
#define R_MIN        8.0f   // ring fully collapsed -> miss
#define PLACE_RADIUS 132    // how far a target center can sit from screen center

// --- Scoring windows on |approach_radius - R_TARGET| (pixels) ---
#define WIN_PERFECT  7.0f
#define WIN_GREAT   18.0f
#define WIN_GOOD    34.0f
#define PTS_PERFECT 300
#define PTS_GREAT   100
#define PTS_GOOD     50

// --- Difficulty / pacing (milliseconds) ---
#define APPROACH_START_MS 1900
#define APPROACH_MIN_MS    650
#define APPROACH_STEP_MS    55   // shaved off per successful hit
#define SPAWN_GAP_MS       300   // pause between rings
#define FEEDBACK_MS        650   // how long a "PERFECT/Miss" flash lingers
#define COUNTDOWN_MS      3000   // 3..2..1
#define GO_MS              500   // "GO!" hold before play starts

#define COL_BG      0x07070C
#define COL_TARGET  0x3FB950   // green bullseye
#define COL_APPROACH 0x29C7C7  // cyan shrinking ring
#define COL_PIP_ON  0xE8533F   // life remaining
#define COL_PIP_OFF 0x2A2A33   // life lost

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

BullseyeApp *BullseyeApp::_instance = nullptr;

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

// Uniform random float in [0, 1).
static float frand(void) { return (float)esp_random() / 4294967296.0f; }

BullseyeApp *BullseyeApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new BullseyeApp();
    }
    return _instance;
}

BullseyeApp::BullseyeApp(): App(APP_NAME, &icon_bullseye, true, false, false) {}
BullseyeApp::~BullseyeApp() {}

// Make `obj` a ring of the given pixel radius centered on (_cx, _cy).
void BullseyeApp::layoutCircle(lv_obj_t *obj, float radius)
{
    int d = (int)(radius * 2.0f);
    if (d < 2) {
        d = 2;
    }
    lv_obj_set_size(obj, d, d);
    lv_obj_align(obj, LV_ALIGN_CENTER, _cx - SCREEN_CX, _cy - SCREEN_CY);
}

bool BullseyeApp::run(void)
{
    ESP_UTILS_LOGI("bullseye run");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // The whole screen is the hit area. The back gesture still works: Brookesia
    // reads the touch device directly, independent of LVGL click routing.
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, onPress, LV_EVENT_PRESSED, this);

    // Target (bullseye): a filled-ish ring at the destination radius.
    _target = lv_obj_create(scr);
    lv_obj_set_style_radius(_target, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(_target, lv_color_hex(COL_TARGET), 0);
    lv_obj_set_style_bg_opa(_target, LV_OPA_30, 0);
    lv_obj_set_style_border_color(_target, lv_color_hex(COL_TARGET), 0);
    lv_obj_set_style_border_width(_target, 4, 0);
    lv_obj_set_style_pad_all(_target, 0, 0);
    lv_obj_clear_flag(_target, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_target, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_target, LV_OBJ_FLAG_HIDDEN);

    // Approach ring: hollow, thick bright border, shrinks each frame.
    _approach = lv_obj_create(scr);
    lv_obj_set_style_radius(_approach, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(_approach, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(_approach, lv_color_hex(COL_APPROACH), 0);
    lv_obj_set_style_border_width(_approach, 6, 0);
    lv_obj_set_style_pad_all(_approach, 0, 0);
    lv_obj_clear_flag(_approach, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_approach, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_approach, LV_OBJ_FLAG_HIDDEN);

    // Score, top center.
    _score_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_score_label, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(_score_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(_score_label, LV_ALIGN_TOP_MID, 0, 58);
    lv_label_set_text(_score_label, "0");

    // Life pips, in a row just under the score.
    for (int i = 0; i < 3; ++i) {
        _life_pip[i] = lv_obj_create(scr);
        lv_obj_set_size(_life_pip[i], 18, 18);
        lv_obj_set_style_radius(_life_pip[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(_life_pip[i], 0, 0);
        lv_obj_set_style_pad_all(_life_pip[i], 0, 0);
        lv_obj_set_style_bg_color(_life_pip[i], lv_color_hex(COL_PIP_ON), 0);
        lv_obj_clear_flag(_life_pip[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(_life_pip[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(_life_pip[i], LV_ALIGN_TOP_MID, (i - 1) * 28, 112);
    }

    // Feedback flash (PERFECT / GREAT / Miss), below the bullseye band.
    _feedback = lv_label_create(scr);
    lv_obj_set_style_text_font(_feedback, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_feedback, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(_feedback, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_feedback, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_label_set_text(_feedback, "");

    // Big center text: countdown digits and the game-over banner.
    _big = lv_label_create(scr);
    lv_obj_set_style_text_font(_big, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(_big, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_big, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(_big, "");

    resetGame();

    _timer = lv_timer_create(onTick, 16, this);
    return true;
}

bool BullseyeApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void BullseyeApp::resetGame(void)
{
    _phase = Phase::Countdown;
    _score = 0;
    _lives = 3;
    _hits = 0;
    _circle_active = false;
    _next_spawn_us = 0;
    _feedback_until_us = 0;
    _last_countdown_shown = -1;
    _countdown_start_us = now_us();

    lv_label_set_text(_score_label, "0");
    lv_label_set_text(_feedback, "");
    lv_obj_add_flag(_target, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_approach, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_style_bg_color(_life_pip[i], lv_color_hex(COL_PIP_ON), 0);
        lv_obj_clear_flag(_life_pip[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void BullseyeApp::spawnCircle(void)
{
    // Area-uniform random point within PLACE_RADIUS of the screen center.
    float ang = frand() * 6.2831853f;
    float dist = PLACE_RADIUS * sqrtf(frand());
    _cx = SCREEN_CX + (int)(dist * cosf(ang));
    _cy = SCREEN_CY + (int)(dist * sinf(ang));

    int dur = APPROACH_START_MS - _hits * APPROACH_STEP_MS;
    if (dur < APPROACH_MIN_MS) {
        dur = APPROACH_MIN_MS;
    }
    _circle_duration_ms = (uint32_t)dur;
    _circle_start_us = now_us();
    _circle_active = true;

    layoutCircle(_target, R_TARGET);
    layoutCircle(_approach, R_MAX);
    lv_obj_clear_flag(_target, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_approach, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_approach);
}

// Linear shrink from R_MAX to R_MIN over the ring's duration.
float BullseyeApp::approachRadius(uint32_t now_us_) const
{
    float t = (float)(now_us_ - _circle_start_us) / (_circle_duration_ms * 1000.0f);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return R_MAX - (R_MAX - R_MIN) * t;
}

void BullseyeApp::registerHit(int points, const char *label, uint32_t color)
{
    _score += points;
    _hits++;
    lv_label_set_text_fmt(_score_label, "%d", _score);
    lv_label_set_text_fmt(_feedback, "%s +%d", label, points);
    lv_obj_set_style_text_color(_feedback, lv_color_hex(color), 0);
    _feedback_until_us = now_us() + FEEDBACK_MS * 1000;

    _circle_active = false;
    lv_obj_add_flag(_target, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_approach, LV_OBJ_FLAG_HIDDEN);
    _next_spawn_us = now_us() + SPAWN_GAP_MS * 1000;
}

void BullseyeApp::registerMiss(const char *label)
{
    _circle_active = false;
    lv_obj_add_flag(_target, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_approach, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(_feedback, label);
    lv_obj_set_style_text_color(_feedback, lv_color_hex(COL_PIP_ON), 0);
    _feedback_until_us = now_us() + FEEDBACK_MS * 1000;

    if (_lives > 0) {
        _lives--;
        lv_obj_set_style_bg_color(_life_pip[_lives], lv_color_hex(COL_PIP_OFF), 0);
    }
    if (_lives <= 0) {
        enterGameOver();
    } else {
        _next_spawn_us = now_us() + SPAWN_GAP_MS * 1000;
    }
}

void BullseyeApp::enterGameOver(void)
{
    _phase = Phase::GameOver;
    _circle_active = false;
    lv_obj_add_flag(_target, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_approach, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(_big, "GAME\nOVER");
    lv_obj_set_style_text_color(_big, lv_color_hex(COL_PIP_ON), 0);
    lv_label_set_text_fmt(_feedback, "Score %d  -  tap to replay", _score);
    lv_obj_set_style_text_color(_feedback, lv_color_hex(0xE6E6F0), 0);
    _feedback_until_us = 0;  // keep it on screen
}

void BullseyeApp::onPress(lv_event_t *e)
{
    BullseyeApp *self = static_cast<BullseyeApp *>(lv_event_get_user_data(e));
    if (self) {
        self->handlePress();
    }
}

void BullseyeApp::handlePress(void)
{
    switch (_phase) {
    case Phase::Countdown:
        return;  // no early taps
    case Phase::GameOver:
        resetGame();
        return;
    case Phase::Playing:
        break;
    }

    if (!_circle_active) {
        return;  // nothing to hit during the inter-ring gap
    }

    float r = approachRadius(now_us());
    float diff = fabsf(r - R_TARGET);
    if (diff <= WIN_PERFECT) {
        registerHit(PTS_PERFECT, "PERFECT", 0xF2C744);
    } else if (diff <= WIN_GREAT) {
        registerHit(PTS_GREAT, "GREAT", COL_APPROACH);
    } else if (diff <= WIN_GOOD) {
        registerHit(PTS_GOOD, "GOOD", 0x9A9AB0);
    } else {
        registerMiss("TOO EARLY");
    }
}

void BullseyeApp::onTick(lv_timer_t *timer)
{
    BullseyeApp *self = static_cast<BullseyeApp *>(lv_timer_get_user_data(timer));
    if (self) {
        self->tick();
    }
}

void BullseyeApp::tick(void)
{
    uint32_t now = now_us();

    // Clear an expired feedback flash (not on the game-over screen).
    if (_phase == Phase::Playing && _feedback_until_us != 0 && now >= _feedback_until_us) {
        lv_label_set_text(_feedback, "");
        _feedback_until_us = 0;
    }

    switch (_phase) {
    case Phase::Countdown: {
        uint32_t elapsed_ms = (now - _countdown_start_us) / 1000;
        if (elapsed_ms < COUNTDOWN_MS) {
            int n = 3 - (int)(elapsed_ms / 1000);  // 3, 2, 1
            if (n != _last_countdown_shown) {
                lv_label_set_text_fmt(_big, "%d", n);
                lv_obj_set_style_text_color(_big, lv_color_hex(0xFFFFFF), 0);
                _last_countdown_shown = n;
            }
        } else if (elapsed_ms < COUNTDOWN_MS + GO_MS) {
            if (_last_countdown_shown != 0) {
                lv_label_set_text(_big, "GO!");
                lv_obj_set_style_text_color(_big, lv_color_hex(COL_TARGET), 0);
                _last_countdown_shown = 0;
            }
        } else {
            lv_label_set_text(_big, "");
            _phase = Phase::Playing;
            _next_spawn_us = now;  // spawn the first ring immediately
        }
        break;
    }
    case Phase::Playing: {
        if (_circle_active) {
            float r = approachRadius(now);
            layoutCircle(_approach, r);
            // Collapsed past the target without a tap -> miss.
            if (r <= R_MIN + 0.01f) {
                registerMiss("MISS");
            }
        } else if (_next_spawn_us != 0 && now >= _next_spawn_us) {
            _next_spawn_us = 0;
            spawnCircle();
        }
        break;
    }
    case Phase::GameOver:
        break;
    }
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, BullseyeApp, APP_NAME, []()
{
    return std::shared_ptr<BullseyeApp>(BullseyeApp::requestInstance(), [](BullseyeApp *) {});
})

} // namespace esp_brookesia::apps
