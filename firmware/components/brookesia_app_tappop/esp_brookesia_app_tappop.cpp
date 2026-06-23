/*
 * Tap Pop game — see header. Round 466x466 AMOLED.
 *
 * Flow: a 30-second, failure-free round. Up to 3 cheerful poppers pop up at
 * random spots inside the round play area; tap one to score +1 and pop it with a
 * happy sparkle. Poppers spawn a little faster and linger a little shorter as the
 * round goes on, but it always stays gentle. When the clock runs out, a "Great
 * job!" banner shows the final score and a tap anywhere starts a fresh round.
 */
#include "esp_brookesia_app_tappop.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:tappop"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"
#include "game_icons.h"

#include <cmath>
#include <cstring>

#define APP_NAME "Tap Pop"

// --- Geometry (screen is 466x466, center 233,233, visible radius ~233) ---
#define SCREEN_CX 233
#define SCREEN_CY 233
#define POPPER_D     92     // popper diameter
#define POPPER_R     46     // popper radius
#define PLACE_RADIUS 175    // popper center stays within this of screen center

// --- Round pacing (milliseconds) ---
#define ROUND_MS        30000   // a 30-second round
// Lifetime a popper lingers if untapped: eases from "long" toward "short".
#define LIFE_START_MS    2200
#define LIFE_END_MS      1200
// Gap before an empty slot spawns again: eases from "slow" toward "snappy".
#define GAP_START_MS      900
#define GAP_END_MS        420
#define SPARK_MS          550   // how long the "+1" pop flash lingers

#define COL_BG    0x07070C
#define COL_EYE   0x1A1A22   // dark eye / smile color
#define COL_TEXT  0xFFFFFF
#define COL_SPARK 0xFFD93B   // sunny yellow "+1"
#define COL_TIME  0x9DE0FF   // soft blue countdown

// Cheerful popper colors, cycled by slot/spawn count.
static const uint32_t POPPER_COLORS[] = {
    0xFF6B6B, 0x4CD964, 0x4D9DFF, 0xFFD93B, 0xB36BFF, 0xFF9F43,
};
#define POPPER_COLOR_COUNT (sizeof(POPPER_COLORS) / sizeof(POPPER_COLORS[0]))

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

TapPopApp *TapPopApp::_instance = nullptr;

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

// Uniform random float in [0, 1).
static float frand(void) { return (float)esp_random() / 4294967296.0f; }

// Linear ease from a -> b as t goes 0 -> 1 (t is clamped).
static uint32_t lerp_ms(uint32_t a, uint32_t b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return (uint32_t)((float)a + ((float)b - (float)a) * t);
}

TapPopApp *TapPopApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new TapPopApp();
    }
    return _instance;
}

TapPopApp::TapPopApp(): App(APP_NAME, &icon_tappop, true, false, false) {}
TapPopApp::~TapPopApp() {}

bool TapPopApp::run(void)
{
    ESP_UTILS_LOGI("tappop run");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // The whole screen is a press target used only to restart on the game-over
    // screen. The back gesture still works: Brookesia reads the touch device
    // directly, independent of LVGL click routing.
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, onScreenPress, LV_EVENT_PRESSED, this);

    // Build the 3 popper slots once; we only show/hide them after this.
    for (int i = 0; i < SLOTS; ++i) {
        Slot &s = _slots[i];

        s.body = lv_obj_create(scr);
        lv_obj_set_size(s.body, POPPER_D, POPPER_D);
        lv_obj_set_style_radius(s.body, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s.body, 0, 0);
        lv_obj_set_style_pad_all(s.body, 0, 0);
        lv_obj_set_style_bg_opa(s.body, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s.body, lv_color_hex(POPPER_COLORS[i]), 0);
        // The popper itself is the tap target.
        lv_obj_add_flag(s.body, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s.body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s.body, onPopperClick, LV_EVENT_CLICKED, this);

        // Two cute eye dots near the top of the face.
        s.eyeL = lv_obj_create(s.body);
        lv_obj_set_size(s.eyeL, 12, 12);
        lv_obj_set_style_radius(s.eyeL, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s.eyeL, 0, 0);
        lv_obj_set_style_pad_all(s.eyeL, 0, 0);
        lv_obj_set_style_bg_opa(s.eyeL, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s.eyeL, lv_color_hex(COL_EYE), 0);
        lv_obj_clear_flag(s.eyeL, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s.eyeL, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(s.eyeL, LV_ALIGN_CENTER, -14, -12);

        s.eyeR = lv_obj_create(s.body);
        lv_obj_set_size(s.eyeR, 12, 12);
        lv_obj_set_style_radius(s.eyeR, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s.eyeR, 0, 0);
        lv_obj_set_style_pad_all(s.eyeR, 0, 0);
        lv_obj_set_style_bg_opa(s.eyeR, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s.eyeR, lv_color_hex(COL_EYE), 0);
        lv_obj_clear_flag(s.eyeR, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s.eyeR, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(s.eyeR, LV_ALIGN_CENTER, 14, -12);

        // A tiny rounded smile bar.
        s.smile = lv_obj_create(s.body);
        lv_obj_set_size(s.smile, 30, 8);
        lv_obj_set_style_radius(s.smile, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s.smile, 0, 0);
        lv_obj_set_style_pad_all(s.smile, 0, 0);
        lv_obj_set_style_bg_opa(s.smile, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s.smile, lv_color_hex(COL_EYE), 0);
        lv_obj_clear_flag(s.smile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(s.smile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(s.smile, LV_ALIGN_CENTER, 0, 14);

        lv_obj_add_flag(s.body, LV_OBJ_FLAG_HIDDEN);
    }

    // Score, big at top center.
    _score_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_score_label, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(_score_label, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(_score_label, LV_ALIGN_TOP_MID, 0, 54);
    lv_label_set_text(_score_label, "0");

    // Seconds remaining, small, just under the score.
    _time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_time_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_time_label, lv_color_hex(COL_TIME), 0);
    lv_obj_align(_time_label, LV_ALIGN_TOP_MID, 0, 110);
    lv_label_set_text(_time_label, "30");

    // "+1" sparkle flash near the bottom band.
    _spark = lv_label_create(scr);
    lv_obj_set_style_text_font(_spark, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(_spark, lv_color_hex(COL_SPARK), 0);
    lv_obj_set_style_text_align(_spark, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_spark, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_label_set_text(_spark, "");

    // Big center banner: the "Great job!" celebration.
    _big = lv_label_create(scr);
    lv_obj_set_style_text_font(_big, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(_big, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_big, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(_big, "");

    resetGame();

    _timer = lv_timer_create(onTick, 30, this);
    return true;
}

bool TapPopApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void TapPopApp::resetGame(void)
{
    _phase = Phase::Playing;
    _score = 0;
    _spark_until_us = 0;
    _last_secs_shown = -1;
    _round_start_us = now_us();

    lv_label_set_text(_score_label, "0");
    lv_label_set_text(_spark, "");
    lv_label_set_text(_big, "");
    lv_obj_clear_flag(_score_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_time_label, LV_OBJ_FLAG_HIDDEN);

    // All slots start empty, with a small staggered first spawn so they don't
    // all pop at the very same instant.
    for (int i = 0; i < SLOTS; ++i) {
        hidePopper(i);
        _slots[i].next_spawn_us = _round_start_us + (uint32_t)(i * 260) * 1000;
    }
}

// Place slot `slot`'s popper at a fresh random spot and show it.
void TapPopApp::spawnPopper(int slot, uint32_t now)
{
    Slot &s = _slots[slot];

    // Area-uniform random point within PLACE_RADIUS of the screen center, so the
    // whole circle stays inside the visible round glass.
    float ang = frand() * 6.2831853f;
    float dist = PLACE_RADIUS * sqrtf(frand());
    int cx = SCREEN_CX + (int)(dist * cosf(ang));
    int cy = SCREEN_CY + (int)(dist * sinf(ang));

    // Cycle through the cheerful palette so each popper feels distinct.
    static uint32_t spawn_count = 0;
    uint32_t color = POPPER_COLORS[spawn_count % POPPER_COLOR_COUNT];
    spawn_count++;
    lv_obj_set_style_bg_color(s.body, lv_color_hex(color), 0);

    // Progress through the round eases difficulty (still gentle).
    float t = (float)(now - _round_start_us) / (float)(ROUND_MS * 1000);
    s.life_ms = lerp_ms(LIFE_START_MS, LIFE_END_MS, t);
    s.spawn_us = now;
    s.active = true;

    lv_obj_align(s.body, LV_ALIGN_CENTER, cx - SCREEN_CX, cy - SCREEN_CY);
    lv_obj_clear_flag(s.body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s.body);
}

void TapPopApp::hidePopper(int slot)
{
    Slot &s = _slots[slot];
    s.active = false;
    lv_obj_add_flag(s.body, LV_OBJ_FLAG_HIDDEN);
}

// A tapped popper: score, pop it away, and flash a happy "+1".
void TapPopApp::popSlot(int slot)
{
    Slot &s = _slots[slot];
    if (!s.active) {
        return;
    }
    _score += 1;
    lv_label_set_text_fmt(_score_label, "%d", _score);

    lv_label_set_text(_spark, "+1");
    _spark_until_us = now_us() + SPARK_MS * 1000;

    hidePopper(slot);
    // Schedule this slot's next pop after a (shrinking) friendly gap.
    float t = (float)(now_us() - _round_start_us) / (float)(ROUND_MS * 1000);
    uint32_t gap = lerp_ms(GAP_START_MS, GAP_END_MS, t);
    s.next_spawn_us = now_us() + gap * 1000;
}

void TapPopApp::enterGameOver(void)
{
    _phase = Phase::GameOver;

    // Tuck every popper away and clear the transient HUD.
    for (int i = 0; i < SLOTS; ++i) {
        hidePopper(i);
    }
    lv_label_set_text(_spark, "");
    _spark_until_us = 0;
    lv_obj_add_flag(_time_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_score_label, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text_fmt(_big, "Great job!\nScore %d\n\ntap to play again", _score);
    lv_obj_set_style_text_color(_big, lv_color_hex(COL_TEXT), 0);
}

void TapPopApp::onPopperClick(lv_event_t *e)
{
    TapPopApp *self = static_cast<TapPopApp *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    if (self->_phase != Phase::Playing) {
        return;  // ignore stray clicks once the round has ended
    }
    lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
    // Find which slot's body was tapped.
    for (int i = 0; i < SLOTS; ++i) {
        if (self->_slots[i].body == target) {
            self->popSlot(i);
            return;
        }
    }
}

void TapPopApp::onScreenPress(lv_event_t *e)
{
    TapPopApp *self = static_cast<TapPopApp *>(lv_event_get_user_data(e));
    if (self) {
        self->handleScreenPress();
    }
}

void TapPopApp::handleScreenPress(void)
{
    // Empty-screen taps do nothing during play; on the celebration screen, a tap
    // anywhere starts a fresh round.
    if (_phase == Phase::GameOver) {
        resetGame();
    }
}

void TapPopApp::onTick(lv_timer_t *timer)
{
    TapPopApp *self = static_cast<TapPopApp *>(lv_timer_get_user_data(timer));
    if (self) {
        self->tick();
    }
}

void TapPopApp::tick(void)
{
    if (_phase != Phase::Playing) {
        return;
    }

    uint32_t now = now_us();

    // Clear an expired "+1" sparkle.
    if (_spark_until_us != 0 && now >= _spark_until_us) {
        lv_label_set_text(_spark, "");
        _spark_until_us = 0;
    }

    // Round clock: show whole seconds remaining, end when it hits zero.
    uint32_t elapsed_ms = (now - _round_start_us) / 1000;
    if (elapsed_ms >= ROUND_MS) {
        enterGameOver();
        return;
    }
    int secs_left = (int)((ROUND_MS - elapsed_ms + 999) / 1000);  // round up
    if (secs_left != _last_secs_shown) {
        lv_label_set_text_fmt(_time_label, "%d", secs_left);
        _last_secs_shown = secs_left;
    }

    // Manage each slot: expire untapped poppers, spawn into empty slots.
    for (int i = 0; i < SLOTS; ++i) {
        Slot &s = _slots[i];
        if (s.active) {
            if ((now - s.spawn_us) >= s.life_ms * 1000) {
                hidePopper(i);  // gently disappears, no penalty
                float t = (float)(now - _round_start_us) / (float)(ROUND_MS * 1000);
                uint32_t gap = lerp_ms(GAP_START_MS, GAP_END_MS, t);
                s.next_spawn_us = now + gap * 1000;
            }
        } else if (s.next_spawn_us != 0 && now >= s.next_spawn_us) {
            s.next_spawn_us = 0;
            spawnPopper(i, now);
        }
    }
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, TapPopApp, APP_NAME, []()
{
    return std::shared_ptr<TapPopApp>(TapPopApp::requestInstance(), [](TapPopApp *) {});
})

} // namespace esp_brookesia::apps
