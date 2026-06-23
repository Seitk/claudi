/*
 * Color Match game — see header. Round 466x466 AMOLED.
 *
 * Flow: boot shows "Tap to start" → Showing: the sequence plays back, lighting
 * each pad bright (~450ms) then dim with a (~250ms) gap; pad taps are ignored.
 * → Input: the kid taps the pads in order, each correct tap flashes that pad.
 * Finishing the whole sequence → Celebrate (brief "Nice!"), level++, the sequence
 * grows by one, and a new round plays. A wrong tap → Oops ("Oops! Watch again"),
 * then the SAME sequence replays at the SAME level (gentle, non-punishing). The
 * highest level reached is kept as the best score.
 *
 * All timing is driven from one ~30ms lv_timer loop using esp_timer timestamps;
 * the pad objects are created once in run().
 */
#include "esp_brookesia_app_colormatch.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:colormatch"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"
#include "game_icons.h"

#include <cmath>
#include <cstring>

#define APP_NAME "Color Match"

// --- Geometry (screen is 466x466, center 233,233, visible radius ~233) ---
#define SCREEN_CX 233
#define SCREEN_CY 233
#define PAD_SIZE   150          // each pad is 150x150
#define PAD_RADIUS  24          // rounded-square corner radius
#define PAD_OFFSET  82          // center offset for the 2x2 layout (+/- 82)

// --- Pad colors: idle (dim) and lit (bright) per pad index ---
//   0 red   1 green   2 blue   3 yellow
static const uint32_t PAD_DIM[4]    = {0x7A2E2E, 0x265E2E, 0x254E7A, 0x7A6A1E};
static const uint32_t PAD_BRIGHT[4] = {0xFF4D4D, 0x4CD964, 0x4D9DFF, 0xFFD93B};

// 2x2 layout: pad i sits at one of these center offsets from screen center.
//   0 = top-left, 1 = top-right, 2 = bottom-left, 3 = bottom-right
static const int PAD_DX[4] = {-PAD_OFFSET,  PAD_OFFSET, -PAD_OFFSET,  PAD_OFFSET};
static const int PAD_DY[4] = {-PAD_OFFSET, -PAD_OFFSET,  PAD_OFFSET,  PAD_OFFSET};

// --- Pacing (milliseconds) ---
#define LIT_MS        450      // how long each pad stays bright during playback
#define GAP_MS        250      // dim gap between playback steps
#define TAP_FLASH_MS  220      // a pad's bright flash when correctly tapped
#define CELEBRATE_MS  800      // "Nice!" hold before the next round
#define OOPS_MS      1100      // "Oops! Watch again" hold before replay
#define START_DELAY_MS 350     // small beat before the very first playback

#define COL_BG       0x07070C
#define COL_TEXT     0xFFFFFF
#define COL_STATUS   0xBFC4D6
#define COL_CELEBR   0x4CD964  // green "Nice!"
#define COL_OOPS     0xFF8A5C  // warm, friendly orange — not alarming red

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

ColorMatchApp *ColorMatchApp::_instance = nullptr;

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

ColorMatchApp *ColorMatchApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new ColorMatchApp();
    }
    return _instance;
}

ColorMatchApp::ColorMatchApp(): App(APP_NAME, &icon_colormatch, true, false, false) {}
ColorMatchApp::~ColorMatchApp() {}

void ColorMatchApp::setPadLit(int pad, bool lit)
{
    if (pad < 0 || pad >= PAD_COUNT || _pad[pad] == nullptr) {
        return;
    }
    uint32_t col = lit ? PAD_BRIGHT[pad] : PAD_DIM[pad];
    lv_obj_set_style_bg_color(_pad[pad], lv_color_hex(col), 0);
    // Lit pads get a subtle bright border too for extra pop.
    lv_obj_set_style_border_color(_pad[pad], lv_color_hex(PAD_BRIGHT[pad]), 0);
    lv_obj_set_style_border_width(_pad[pad], lit ? 4 : 0, 0);
}

void ColorMatchApp::updateLevelLabel(void)
{
    lv_label_set_text_fmt(_level_label, "Level %d", _level);
}

bool ColorMatchApp::run(void)
{
    ESP_UTILS_LOGI("colormatch run");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Level / score, top center.
    _level_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_level_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_level_label, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(_level_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_level_label, LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(_level_label, "Level 1");

    // Status line just under the level ("Watch!" / "Your turn!").
    _status_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_status_label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(_status_label, lv_color_hex(COL_STATUS), 0);
    lv_obj_set_style_text_align(_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_status_label, LV_ALIGN_TOP_MID, 0, 78);
    lv_label_set_text(_status_label, "");

    // Four pads in a 2x2 grid, each clickable and tagged with its index.
    for (int i = 0; i < PAD_COUNT; ++i) {
        _pad[i] = lv_obj_create(scr);
        lv_obj_set_size(_pad[i], PAD_SIZE, PAD_SIZE);
        lv_obj_set_style_radius(_pad[i], PAD_RADIUS, 0);
        lv_obj_set_style_pad_all(_pad[i], 0, 0);
        lv_obj_set_style_border_width(_pad[i], 0, 0);
        lv_obj_set_style_bg_color(_pad[i], lv_color_hex(PAD_DIM[i]), 0);
        lv_obj_set_style_bg_opa(_pad[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(_pad[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_pad[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(_pad[i], LV_ALIGN_CENTER, PAD_DX[i], PAD_DY[i]);
        // Encode the pad index in the user data; cast back inside the cb.
        lv_obj_add_event_cb(_pad[i], onPadPress, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
    }

    // Big center "Tap to start" prompt, shown at boot over the (dim) pads.
    _prompt = lv_label_create(scr);
    lv_obj_set_style_text_font(_prompt, &lv_font_montserrat_34, 0);
    lv_obj_set_style_text_color(_prompt, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(_prompt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_prompt, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(_prompt, "Tap to\nstart!");
    lv_obj_add_flag(_prompt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_prompt, onPromptPress, LV_EVENT_CLICKED, this);

    resetGame();

    _timer = lv_timer_create(onTick, 30, this);
    return true;
}

bool ColorMatchApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void ColorMatchApp::resetGame(void)
{
    _phase = Phase::Idle;
    _level = 1;
    _best = 1;
    _seq_len = 0;
    _play_index = 0;
    _play_on = false;
    _input_index = 0;
    _flash_pad = -1;
    _flash_until_us = 0;
    _phase_until_us = 0;

    for (int i = 0; i < PAD_COUNT; ++i) {
        setPadLit(i, false);
    }
    updateLevelLabel();
    lv_label_set_text(_status_label, "");
    // Show the boot prompt; the loop waits in Idle until it is tapped.
    lv_label_set_text(_prompt, "Tap to\nstart!");
    lv_obj_clear_flag(_prompt, LV_OBJ_FLAG_HIDDEN);
}

void ColorMatchApp::extendSequence(void)
{
    if (_seq_len < SEQ_MAX) {
        _seq[_seq_len] = (int)(esp_random() % PAD_COUNT);
        _seq_len++;
    }
}

void ColorMatchApp::startRound(void)
{
    // (Re)play the current sequence from the top.
    _phase = Phase::Showing;
    _play_index = 0;
    _play_on = false;
    _input_index = 0;
    _flash_pad = -1;
    for (int i = 0; i < PAD_COUNT; ++i) {
        setPadLit(i, false);
    }
    lv_obj_add_flag(_prompt, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(_status_label, "Watch!");
    lv_obj_set_style_text_color(_status_label, lv_color_hex(COL_STATUS), 0);
    updateLevelLabel();
    // Small beat (all dim) before the first pad lights up.
    _play_phase_until_us = now_us() + START_DELAY_MS * 1000;
}

void ColorMatchApp::enterInput(void)
{
    _phase = Phase::Input;
    _input_index = 0;
    _flash_pad = -1;
    for (int i = 0; i < PAD_COUNT; ++i) {
        setPadLit(i, false);
    }
    lv_label_set_text(_status_label, "Your turn!");
    lv_obj_set_style_text_color(_status_label, lv_color_hex(COL_CELEBR), 0);
}

void ColorMatchApp::enterCelebrate(void)
{
    _phase = Phase::Celebrate;
    // A correct-tap flash may still be pending; we re-light every pad below, so
    // cancel it to avoid one pad dimming mid-celebration when its flash expires.
    _flash_pad = -1;
    // Whole sequence correct: bump level/score, track best, light all pads.
    _level++;
    if (_level > _best) {
        _best = _level;
    }
    updateLevelLabel();
    for (int i = 0; i < PAD_COUNT; ++i) {
        setPadLit(i, true);
    }
    lv_label_set_text(_status_label, "Nice!");
    lv_obj_set_style_text_color(_status_label, lv_color_hex(COL_CELEBR), 0);
    _phase_until_us = now_us() + CELEBRATE_MS * 1000;
}

void ColorMatchApp::enterOops(void)
{
    _phase = Phase::Oops;
    // Cancel any pending correct-tap flash so it cannot dim one pad mid-Oops.
    _flash_pad = -1;
    // Gentle: flash all pads bright, friendly message, then replay SAME sequence
    // at the SAME level (no level change).
    for (int i = 0; i < PAD_COUNT; ++i) {
        setPadLit(i, true);
    }
    lv_label_set_text(_status_label, "Oops! Watch again");
    lv_obj_set_style_text_color(_status_label, lv_color_hex(COL_OOPS), 0);
    _phase_until_us = now_us() + OOPS_MS * 1000;
}

void ColorMatchApp::flashPad(int pad, uint32_t now)
{
    _flash_pad = pad;
    _flash_until_us = now + TAP_FLASH_MS * 1000;
    setPadLit(pad, true);
}

void ColorMatchApp::onPromptPress(lv_event_t *e)
{
    ColorMatchApp *self = static_cast<ColorMatchApp *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    // Only meaningful at boot, while idle and waiting to begin.
    if (self->_phase == Phase::Idle) {
        self->_seq_len = 0;
        self->extendSequence();   // first sequence: length 1
        self->startRound();
    }
}

void ColorMatchApp::onPadPress(lv_event_t *e)
{
    if (_instance == nullptr) {
        return;
    }
    int pad = (int)(intptr_t)lv_event_get_user_data(e);
    _instance->handlePadPress(pad);
}

void ColorMatchApp::handlePadPress(int pad)
{
    // Pad taps only count during the kid's Input turn.
    if (_phase != Phase::Input) {
        return;
    }
    if (pad < 0 || pad >= PAD_COUNT) {
        return;
    }

    if (pad == _seq[_input_index]) {
        // Correct: flash this pad and advance.
        flashPad(pad, now_us());
        _input_index++;
        if (_input_index >= _seq_len) {
            // Whole sequence reproduced — celebrate and grow.
            enterCelebrate();
        }
    } else {
        // Wrong tap: gentle retry of the same sequence.
        enterOops();
    }
}

void ColorMatchApp::onTick(lv_timer_t *timer)
{
    ColorMatchApp *self = static_cast<ColorMatchApp *>(lv_timer_get_user_data(timer));
    if (self) {
        self->tick();
    }
}

void ColorMatchApp::tick(void)
{
    uint32_t now = now_us();

    // A correct-tap pad flash expires independently of the phase machine.
    if (_flash_pad >= 0 && now >= _flash_until_us) {
        setPadLit(_flash_pad, false);
        _flash_pad = -1;
    }

    switch (_phase) {
    case Phase::Idle:
        break;  // waiting for the "Tap to start" prompt

    case Phase::Showing: {
        if (now < _play_phase_until_us) {
            break;  // still inside the current lit / gap window
        }
        if (!_play_on) {
            // Was in a gap (or the initial delay): light the next pad.
            if (_play_index >= _seq_len) {
                // Whole sequence shown — hand over to the kid.
                enterInput();
                break;
            }
            setPadLit(_seq[_play_index], true);
            _play_on = true;
            _play_phase_until_us = now + LIT_MS * 1000;
        } else {
            // A pad was lit: dim it and start the gap before the next step.
            setPadLit(_seq[_play_index], false);
            _play_on = false;
            _play_index++;
            _play_phase_until_us = now + GAP_MS * 1000;
        }
        break;
    }

    case Phase::Input:
        break;  // driven entirely by pad taps

    case Phase::Celebrate: {
        if (now >= _phase_until_us) {
            // Grow the sequence and replay the new, longer round.
            extendSequence();
            startRound();
        }
        break;
    }

    case Phase::Oops: {
        if (now >= _phase_until_us) {
            // Replay the SAME sequence at the SAME level.
            startRound();
        }
        break;
    }
    }
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, ColorMatchApp, APP_NAME, []()
{
    return std::shared_ptr<ColorMatchApp>(ColorMatchApp::requestInstance(), [](ColorMatchApp *) {});
})

} // namespace esp_brookesia::apps
