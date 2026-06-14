/*
 * Multiply game — see header. Gentle single-digit multiplication for kids.
 * Responsive: geometry comes from the live display via claudi_ui_layout, so the
 * same code lays out on the round AMOLED and the rectangular watch.
 */
#include "esp_brookesia_app_multiply.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:multiply"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"

#include "claudi_ui_layout.h"

#define APP_NAME "Multiply"

// --- Palette ---
#define COL_BG       0x07070C
#define COL_CHOICE   0x21304A   // calm idle button
#define COL_OK       0x2E7D46   // correct: green
#define COL_BAD      0xB5532A   // wrong: warm orange (gentle, not alarming red)
#define COL_TEXT     0xFFFFFF
#define COL_PROBLEM  0xFFE08A   // warm yellow problem text
#define COL_SCORE    0x9DE0FF   // soft blue score

// --- Pacing (milliseconds) ---
#define FEEDBACK_MS     500     // green hold after a correct tap, before next problem
#define WRONG_FLASH_MS  350     // warm-orange flash on a wrong tap

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

MultiplyApp *MultiplyApp::_instance = nullptr;

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

MultiplyApp *MultiplyApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new MultiplyApp();
    }
    return _instance;
}

MultiplyApp::MultiplyApp(): App(APP_NAME, nullptr, true, false, false) {}
MultiplyApp::~MultiplyApp() {}

void MultiplyApp::setChoiceColor(int idx, uint32_t bg)
{
    if (idx < 0 || idx >= CHOICE_COUNT || _choice[idx] == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(_choice[idx], lv_color_hex(bg), 0);
}

void MultiplyApp::updateScore(void)
{
    lv_label_set_text_fmt(_score_label, "Score %d", _score);
}

void MultiplyApp::nextProblem(void)
{
    // Single-digit factors (1..9); product is 1..81.
    _op = 'x';
    _a = 1 + (int)(esp_random() % 9);
    _b = 1 + (int)(esp_random() % 9);
    _answer = _a * _b;

    // Build four unique choices including the answer, then shuffle. Distractors
    // include the off-by-a-row table confusions (+/- a, +/- b) plus +/-1, +/-2.
    const int cands[] = {
        _answer + _a, _answer - _a, _answer + _b, _answer - _b,
        _answer + 1, _answer - 1, _answer + 2, _answer - 2,
    };
    const int NCAND = (int)(sizeof(cands) / sizeof(cands[0]));
    _vals[0] = _answer;
    int n = 1;
    int guard = 0;
    while (n < CHOICE_COUNT && guard < 200) {
        guard++;
        int v = cands[(int)(esp_random() % NCAND)];
        if (v < 0) {
            continue;
        }
        bool dup = false;
        for (int i = 0; i < n; ++i) {
            if (_vals[i] == v) { dup = true; break; }
        }
        if (!dup) {
            _vals[n++] = v;
        }
    }
    // Fallback if the candidate pool couldn't fill 4 uniques (small products).
    int filler = _answer + 3;
    while (n < CHOICE_COUNT) {
        bool dup = false;
        for (int i = 0; i < n; ++i) {
            if (_vals[i] == filler) { dup = true; break; }
        }
        if (!dup && filler >= 0) {
            _vals[n++] = filler;
        }
        filler++;
    }

    for (int i = CHOICE_COUNT - 1; i > 0; --i) {
        int j = (int)(esp_random() % (i + 1));
        int t = _vals[i]; _vals[i] = _vals[j]; _vals[j] = t;
    }
    for (int i = 0; i < CHOICE_COUNT; ++i) {
        if (_vals[i] == _answer) { _correct = i; }
    }

    lv_label_set_text_fmt(_problem_label, "%d %c %d = ?", _a, _op, _b);
    for (int i = 0; i < CHOICE_COUNT; ++i) {
        lv_label_set_text_fmt(_choice_label[i], "%d", _vals[i]);
        setChoiceColor(i, COL_CHOICE);
    }
    _wrong_pad = -1;
}

bool MultiplyApp::run(void)
{
    ESP_UTILS_LOGI("multiply run");

    lv_display_t *disp = lv_display_get_default();
    int16_t W = (int16_t)lv_display_get_horizontal_resolution(disp);
    int16_t H = (int16_t)lv_display_get_vertical_resolution(disp);
    claudi_layout_t L = claudi_layout_compute(W, H, CLAUDI_LAYOUT_RECT);

    const bool big = (L.short_side >= 360);
    const lv_font_t *f_problem = big ? &lv_font_montserrat_36 : &lv_font_montserrat_24;
    const lv_font_t *f_choice  = big ? &lv_font_montserrat_34 : &lv_font_montserrat_22;
    const lv_font_t *f_score   = big ? &lv_font_montserrat_22 : &lv_font_montserrat_16;
    const int csize = (int)(L.short_side * 0.30f);
    const int coff  = (int)(L.short_side * 0.18f);
    const int biasY = (int)(H * 0.06f);
    const int cdx[CHOICE_COUNT] = {-coff,  coff, -coff,  coff};
    const int cdy[CHOICE_COUNT] = {-coff, -coff,  coff,  coff};

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    _score_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_score_label, f_score, 0);
    lv_obj_set_style_text_color(_score_label, lv_color_hex(COL_SCORE), 0);
    lv_obj_align(_score_label, LV_ALIGN_TOP_MID, 0, (int)(H * 0.04f));

    _problem_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_problem_label, f_problem, 0);
    lv_obj_set_style_text_color(_problem_label, lv_color_hex(COL_PROBLEM), 0);
    lv_obj_set_style_text_align(_problem_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_problem_label, LV_ALIGN_TOP_MID, 0, (int)(H * 0.15f));

    for (int i = 0; i < CHOICE_COUNT; ++i) {
        _choice[i] = lv_button_create(scr);
        lv_obj_set_size(_choice[i], csize, csize);
        lv_obj_set_style_radius(_choice[i], (int)(csize * 0.28f), 0);
        lv_obj_set_style_bg_color(_choice[i], lv_color_hex(COL_CHOICE), 0);
        lv_obj_align(_choice[i], LV_ALIGN_CENTER, cdx[i], biasY + cdy[i]);
        lv_obj_add_event_cb(_choice[i], onChoicePress, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        _choice_label[i] = lv_label_create(_choice[i]);
        lv_obj_set_style_text_font(_choice_label[i], f_choice, 0);
        lv_obj_set_style_text_color(_choice_label[i], lv_color_hex(COL_TEXT), 0);
        lv_obj_center(_choice_label[i]);
    }

    _phase = Phase::Playing;
    _score = 0;
    _wrong_pad = -1;
    updateScore();
    nextProblem();

    _timer = lv_timer_create(onTick, 30, this);
    return true;
}

bool MultiplyApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void MultiplyApp::onChoicePress(lv_event_t *e)
{
    if (_instance == nullptr) {
        return;
    }
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    _instance->handleChoice(idx);
}

void MultiplyApp::handleChoice(int idx)
{
    if (_phase != Phase::Playing || idx < 0 || idx >= CHOICE_COUNT) {
        return;
    }
    if (idx == _correct) {
        setChoiceColor(idx, COL_OK);
        _score++;
        updateScore();
        _wrong_pad = -1;
        _phase = Phase::Feedback;
        _feedback_until_us = now_us() + FEEDBACK_MS * 1000;
    } else {
        setChoiceColor(idx, COL_BAD);
        _wrong_pad = idx;
        _wrong_until_us = now_us() + WRONG_FLASH_MS * 1000;
    }
}

void MultiplyApp::onTick(lv_timer_t *timer)
{
    MultiplyApp *self = static_cast<MultiplyApp *>(lv_timer_get_user_data(timer));
    if (self) {
        self->tick();
    }
}

void MultiplyApp::tick(void)
{
    uint32_t now = now_us();

    if (_wrong_pad >= 0 && now >= _wrong_until_us) {
        setChoiceColor(_wrong_pad, COL_CHOICE);
        _wrong_pad = -1;
    }

    if (_phase == Phase::Feedback && now >= _feedback_until_us) {
        nextProblem();
        _phase = Phase::Playing;
    }
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, MultiplyApp, APP_NAME, []()
{
    return std::shared_ptr<MultiplyApp>(MultiplyApp::requestInstance(), [](MultiplyApp *) {});
})

} // namespace esp_brookesia::apps
