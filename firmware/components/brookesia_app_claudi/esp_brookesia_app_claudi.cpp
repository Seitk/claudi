/*
 * claudi ESP-Brookesia app — see header. Round 466x466 AMOLED UI:
 *   - state-coloured perimeter ring
 *   - "claudi status" (state word) curved along the top arc
 *   - Wi-Fi + battery icons row near the top
 *   - a floating bubble with the live Claude-session count (shown only when >0)
 *   - the slime pet (per-state 2-frame animation) centered
 *   - bottom status line
 *   - approval card with on-screen Approve/Deny/Skip + the BOOT button
 */
#include "esp_brookesia_app_claudi.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:claudi"
#include "esp_lib_utils.h"

#include "esp_timer.h"

#include <cmath>
#include <cstring>

extern "C" {
#include "claudi_core.h"
#include "claudi_net.h"
#include "claudi_power.h"
#include "iot_button.h"
#include "button_gpio.h"
}

#define CLAUDI_BUTTON_GPIO   0
#define CLAUDI_LONG_PRESS_MS 800

#define SCREEN_CX 233
#define SCREEN_CY 233
#define ARC_RADIUS 196
#define ARC_STEP_DEG 8.0f
#define BATTERY_POLL_MS 5000

#define APP_NAME "claudi"

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

ClaudiApp *ClaudiApp::_instance = nullptr;

struct StateStyle {
    uint32_t body;
    const char *tag;
};

static StateStyle style_for(int st)
{
    switch (st) {
    case CLAUDI_STATE_IDLE:      return {0x3A7BD5, "idle"};
    case CLAUDI_STATE_BLINK:     return {0x3A7BD5, "idle"};
    case CLAUDI_STATE_HAPPY:     return {0xF2C744, "happy"};
    case CLAUDI_STATE_SLEEPY:    return {0x4A4A5A, "sleepy"};
    case CLAUDI_STATE_CURIOUS:   return {0x29C7C7, "curious"};
    case CLAUDI_STATE_ALERT:     return {0xE8533F, "alert"};
    case CLAUDI_STATE_BORED:     return {0x6B6B7B, "bored"};
    case CLAUDI_STATE_WORKING:   return {0x3FB950, "working"};
    case CLAUDI_STATE_THINKING:  return {0x9B6DFF, "thinking"};
    case CLAUDI_STATE_ATTENTION: return {0xE8533F, "needs you"};
    case CLAUDI_STATE_IDEA:      return {0xF2C744, "idea"};
    case CLAUDI_STATE_EXCITED:   return {0xFF8C42, "excited"};
    default:                     return {0x3A7BD5, "idle"};
    }
}

static const char *batt_symbol(int pct, bool charging)
{
    if (charging) return LV_SYMBOL_CHARGE;
    if (pct >= 80) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 55) return LV_SYMBOL_BATTERY_3;
    if (pct >= 30) return LV_SYMBOL_BATTERY_2;
    if (pct >= 10) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// --- Pet art: 2 frames per state (tools/gen_pet_assets.py), C-linkage globals.
extern "C" {
#define DECL2(s) LV_IMAGE_DECLARE(slime_##s##_0); LV_IMAGE_DECLARE(slime_##s##_1);
DECL2(idle) DECL2(blink) DECL2(happy) DECL2(sleepy) DECL2(curious) DECL2(alert)
DECL2(bored) DECL2(working) DECL2(thinking) DECL2(attention) DECL2(idea) DECL2(excited)
#undef DECL2
}

#define PETF(s) { &slime_##s##_0, &slime_##s##_1 }
static const lv_image_dsc_t *kPetFrames[CLAUDI_STATE_COUNT][2] = {
    PETF(idle), PETF(blink), PETF(happy), PETF(sleepy), PETF(curious), PETF(alert),
    PETF(bored), PETF(working), PETF(thinking), PETF(attention), PETF(idea), PETF(excited),
};
#undef PETF

// --- Approval input handlers (no-op unless a request is pending) ---
static void on_touch_approve(lv_event_t *) { claudi_net_post_decision(CLAUDI_DECISION_APPROVE); }
static void on_touch_deny(lv_event_t *)    { claudi_net_post_decision(CLAUDI_DECISION_DENY); }
static void on_touch_dismiss(lv_event_t *) { claudi_net_post_decision(CLAUDI_DECISION_DISMISS); }
static void on_btn_approve(void *, void *) { if (claudi_net_pending()) claudi_net_post_decision(CLAUDI_DECISION_APPROVE); }
static void on_btn_deny(void *, void *)    { if (claudi_net_pending()) claudi_net_post_decision(CLAUDI_DECISION_DENY); }

ClaudiApp *ClaudiApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new ClaudiApp();
    }
    return _instance;
}

ClaudiApp::ClaudiApp(): App(APP_NAME, nullptr, true, false, false) {}
ClaudiApp::~ClaudiApp() {}

static lv_obj_t *make_card_button(lv_obj_t *parent, const char *text, uint32_t bg,
                                  lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 120, 56);
    lv_obj_set_style_radius(btn, 28, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_20, 0);
    lv_obj_center(lab);
    return btn;
}

// A floating circular status bubble with a centered text/glyph label.
static lv_obj_t *make_bubble(lv_obj_t *parent, lv_obj_t **out_label, int size,
                             uint32_t bg, const lv_font_t *font)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lab = lv_label_create(b);
    lv_obj_set_style_text_font(lab, font, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lab);
    *out_label = lab;
    return b;
}

bool ClaudiApp::run(void)
{
    ESP_UTILS_LOGI("claudi run");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x07070C), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Perimeter ring (state colour). Decorative — don't intercept touches.
    _ring = lv_obj_create(scr);
    lv_obj_set_size(_ring, 462, 462);
    lv_obj_center(_ring);
    lv_obj_set_style_radius(_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_ring, 10, 0);
    lv_obj_set_style_border_color(_ring, lv_color_hex(style_for(CLAUDI_STATE_IDLE).body), 0);
    lv_obj_clear_flag(_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_ring, LV_OBJ_FLAG_SCROLLABLE);

    // Pet animation, centered.
    _pet = lv_animimg_create(scr);
    lv_animimg_set_src(_pet, (const void **)kPetFrames[CLAUDI_STATE_IDLE], 2);
    lv_animimg_set_duration(_pet, 700);
    lv_animimg_set_repeat_count(_pet, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(_pet);
    lv_image_set_scale(_pet, 360);
    lv_obj_align(_pet, LV_ALIGN_CENTER, 0, 4);

    // Floating status bubbles near the top: Wi-Fi (left) and battery (right)
    // flank the center; the live-session count sits just below, shown only when
    // sessions > 0.
    _wifi_bubble = make_bubble(scr, &_wifi_label, 52, 0x1A1A22, &lv_font_montserrat_20);
    lv_obj_align(_wifi_bubble, LV_ALIGN_TOP_MID, -74, 92);
    lv_label_set_text(_wifi_label, LV_SYMBOL_WIFI);

    _batt_bubble = make_bubble(scr, &_batt_label, 52, 0x1A1A22, &lv_font_montserrat_20);
    lv_obj_align(_batt_bubble, LV_ALIGN_TOP_MID, 74, 92);
    lv_label_set_text(_batt_label, LV_SYMBOL_BATTERY_FULL);

    _sess_bubble = make_bubble(scr, &_sess_label, 52, 0x2E6BE6, &lv_font_montserrat_22);
    lv_obj_align(_sess_bubble, LV_ALIGN_TOP_MID, 0, 150);
    lv_label_set_text(_sess_label, "0");
    lv_obj_add_flag(_sess_bubble, LV_OBJ_FLAG_HIDDEN);

    // Bottom status line.
    _transcript = lv_label_create(scr);
    lv_obj_set_width(_transcript, 300);
    lv_label_set_long_mode(_transcript, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(_transcript, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_transcript, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_set_style_text_color(_transcript, lv_color_hex(0x9A9AB0), 0);
    lv_obj_set_style_text_font(_transcript, &lv_font_montserrat_18, 0);
    lv_label_set_text(_transcript, "");

    // Approval card: text + Approve/Deny/Skip buttons.
    _card = lv_obj_create(scr);
    lv_obj_set_size(_card, 400, 210);
    lv_obj_align(_card, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_radius(_card, 22, 0);
    lv_obj_set_style_bg_color(_card, lv_color_hex(0x1A1320), 0);
    lv_obj_set_style_border_color(_card, lv_color_hex(0xE8533F), 0);
    lv_obj_set_style_border_width(_card, 2, 0);
    lv_obj_clear_flag(_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_card, LV_OBJ_FLAG_HIDDEN);

    _card_label = lv_label_create(_card);
    lv_obj_set_width(_card_label, 360);
    lv_label_set_long_mode(_card_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(_card_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_card_label, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_set_style_text_color(_card_label, lv_color_hex(0xFFE0DA), 0);
    lv_obj_set_style_text_font(_card_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(_card_label, "");

    lv_obj_t *approve = make_card_button(_card, "Approve", 0x2E7D46, on_touch_approve, this);
    lv_obj_align(approve, LV_ALIGN_BOTTOM_LEFT, 4, -6);
    lv_obj_t *dismiss = make_card_button(_card, "Skip", 0x3A3A48, on_touch_dismiss, this);
    lv_obj_align(dismiss, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_t *deny = make_card_button(_card, "Deny", 0xB23121, on_touch_deny, this);
    lv_obj_align(deny, LV_ALIGN_BOTTOM_RIGHT, -4, -6);

    // BOOT button (GPIO0): short = approve, long = deny. Created once.
    static button_handle_t s_btn = nullptr;
    if (s_btn == nullptr) {
        button_config_t btn_cfg = { .long_press_time = CLAUDI_LONG_PRESS_MS, .short_press_time = 0 };
        button_gpio_config_t gpio_cfg = {
            .gpio_num = CLAUDI_BUTTON_GPIO, .active_level = 0,
            .enable_power_save = false, .disable_pull = false,
        };
        if (iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &s_btn) == ESP_OK && s_btn) {
            iot_button_register_cb(s_btn, BUTTON_SINGLE_CLICK, nullptr, on_btn_approve, this);
            iot_button_register_cb(s_btn, BUTTON_LONG_PRESS_START, nullptr, on_btn_deny, this);
        } else {
            ESP_UTILS_LOGW("BOOT button init failed");
        }
    }

    _arc_cache[0] = '\0';
    _arc_color = 0;
    _card_cache[0] = '\0';
    _last_state = -1;
    _last_sessions = -1;
    _attention_since_ms = 0;
    _last_batt_ms = 0;
    _batt_pct = -1;
    _charging = false;

    _timer = lv_timer_create(onTick, 300, this);
    return true;
}

bool ClaudiApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void ClaudiApp::setArcText(const char *text, uint32_t color)
{
    if (color == _arc_color && strncmp(text, _arc_cache, sizeof(_arc_cache)) == 0) {
        return;
    }
    strncpy(_arc_cache, text, sizeof(_arc_cache) - 1);
    _arc_cache[sizeof(_arc_cache) - 1] = '\0';
    _arc_color = color;

    for (int i = 0; i < kMaxArcChars; ++i) {
        if (_arc_chars[i]) {
            lv_obj_del(_arc_chars[i]);
            _arc_chars[i] = nullptr;
        }
    }
    int n = (int)strlen(text);
    if (n > kMaxArcChars) {
        n = kMaxArcChars;
    }
    float start = -90.0f - (n - 1) * ARC_STEP_DEG / 2.0f;
    lv_obj_t *scr = lv_scr_act();
    for (int i = 0; i < n; ++i) {
        char ch[2] = { text[i], '\0' };
        lv_obj_t *lab = lv_label_create(scr);
        lv_label_set_text(lab, ch);
        lv_obj_set_style_text_color(lab, lv_color_hex(color), 0);
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_24, 0);
        float deg = start + i * ARC_STEP_DEG;
        float rad = deg * 3.14159265f / 180.0f;
        int dx = (int)(ARC_RADIUS * cosf(rad));
        int dy = (int)(ARC_RADIUS * sinf(rad));
        lv_obj_set_style_transform_pivot_x(lab, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(lab, lv_pct(50), 0);
        lv_obj_set_style_transform_rotation(lab, (int)((deg + 90.0f) * 10.0f), 0);
        lv_obj_align(lab, LV_ALIGN_CENTER, dx, dy);
        _arc_chars[i] = lab;
    }
}

void ClaudiApp::onTick(lv_timer_t *timer)
{
    ClaudiApp *self = static_cast<ClaudiApp *>(lv_timer_get_user_data(timer));
    if (self) {
        self->applyState();
    }
}

void ClaudiApp::applyState(void)
{
    claudi_snapshot_t snap;
    claudi_net_get_snapshot(&snap);
    claudi_derived_t d = claudi_derive(&snap, now_ms());

    const bool wifi = claudi_net_is_connected();
    const StateStyle st = style_for(d.effective);

    // Pet animation + ring colour follow the effective state.
    if ((int)d.effective != _last_state) {
        lv_animimg_set_src(_pet, (const void **)kPetFrames[d.effective], 2);
        lv_animimg_start(_pet);
        lv_obj_set_style_border_color(_ring, lv_color_hex(st.body), 0);
        _last_state = (int)d.effective;
    }

    // Curved status (state word only — stable, so it rarely rebuilds).
    setArcText(st.tag, st.body);

    // Wi-Fi bubble: glyph brightens when connected, bubble tints green.
    lv_obj_set_style_text_color(_wifi_label, lv_color_hex(wifi ? 0xE6E6F0 : 0x55555F), 0);
    lv_obj_set_style_bg_color(_wifi_bubble, lv_color_hex(wifi ? 0x1E2A20 : 0x1A1A22), 0);

    // Battery bubble (polled every few seconds; I2C is comparatively slow).
    uint32_t now = now_ms();
    if (_batt_pct < 0 || (now - _last_batt_ms) > BATTERY_POLL_MS) {
        _batt_pct = claudi_power_battery_percent();
        _charging = claudi_power_charging();
        _last_batt_ms = now;
        if (_batt_pct < 0) {
            lv_obj_add_flag(_batt_bubble, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(_batt_bubble, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(_batt_label, batt_symbol(_batt_pct, _charging));
            bool low = (_batt_pct < 15 && !_charging);
            lv_obj_set_style_text_color(_batt_label, lv_color_hex(low ? 0xE8533F : 0xE6E6F0), 0);
            lv_obj_set_style_bg_color(_batt_bubble, lv_color_hex(low ? 0x2A1212 : 0x1A1A22), 0);
        }
    }

    // Floating session-count bubble.
    int sessions = (int)snap.sessions;
    if (sessions != _last_sessions) {
        if (sessions > 0) {
            lv_label_set_text_fmt(_sess_label, "%d", sessions);
            lv_obj_clear_flag(_sess_bubble, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_sess_bubble, LV_OBJ_FLAG_HIDDEN);
        }
        _last_sessions = sessions;
    }

    // Bottom status line.
    lv_label_set_text(_transcript, snap.msg);

    // Approval card.
    const bool needs_human = snap.waiting || snap.prompt.set;
    if (needs_human) {
        if (_attention_since_ms == 0) {
            _attention_since_ms = now;
        }
        uint32_t elapsed_s = (now - _attention_since_ms) / 1000;
        const char *tool = snap.prompt.tool[0] ? snap.prompt.tool : "input";
        const char *hint = snap.prompt.hint[0] ? snap.prompt.hint : snap.msg;
        char text[160];
        snprintf(text, sizeof(text), "approve %s?  (%us)\n%s", tool, (unsigned)elapsed_s, hint);
        if (strncmp(text, _card_cache, sizeof(_card_cache)) != 0) {  // avoid per-tick relayout
            strncpy(_card_cache, text, sizeof(_card_cache) - 1);
            _card_cache[sizeof(_card_cache) - 1] = '\0';
            lv_label_set_text(_card_label, text);
        }
        lv_obj_set_style_border_color(_card, lv_color_hex(elapsed_s >= 10 ? 0xFF2D1A : 0xE8533F), 0);
        if (lv_obj_has_flag(_card, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(_card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(_card);  // ensure the buttons are tappable on top
        }
    } else {
        _attention_since_ms = 0;
        _card_cache[0] = '\0';
        lv_obj_add_flag(_card, LV_OBJ_FLAG_HIDDEN);
    }
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, ClaudiApp, APP_NAME, []()
{
    return std::shared_ptr<ClaudiApp>(ClaudiApp::requestInstance(), [](ClaudiApp *) {});
})

} // namespace esp_brookesia::apps
