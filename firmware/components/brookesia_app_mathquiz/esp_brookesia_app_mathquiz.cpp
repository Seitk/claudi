/*
 * Math game — see header. Gentle 2-digit add & subtract for grade-1 kids.
 * Responsive: geometry comes from the live display via claudi_ui_layout, so the
 * same code lays out on the round AMOLED and the rectangular watch.
 */
#include "esp_brookesia_app_mathquiz.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:mathquiz"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"

#include "claudi_ui_layout.h"

#define APP_NAME "Math"

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

MathQuizApp *MathQuizApp::_instance = nullptr;

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

MathQuizApp *MathQuizApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new MathQuizApp();
    }
    return _instance;
}

MathQuizApp::MathQuizApp(): App(APP_NAME, nullptr, true, false, false) {}
MathQuizApp::~MathQuizApp() {}

void MathQuizApp::setChoiceColor(int idx, uint32_t bg)
{
    if (idx < 0 || idx >= CHOICE_COUNT || _choice[idx] == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(_choice[idx], lv_color_hex(bg), 0);
}

void MathQuizApp::updateScore(void)
{
    lv_label_set_text_fmt(_score_label, "Score %d", _score);
}

void MathQuizApp::nextProblem(void)
{
    if (esp_random() % 2 == 0) {
        // Addition: both operands two-digit, sum kept <= 99.
        _op = '+';
        _a = 10 + (int)(esp_random() % 80);        // 10..89
        _b = 10 + (int)(esp_random() % (90 - _a)); // 10..(99 - _a)
        _answer = _a + _b;
    } else {
        // Subtraction: two-digit operands, result never negative.
        _op = '-';
        _a = 20 + (int)(esp_random() % 80);        // 20..99 (minuend)
        _b = 10 + (int)(esp_random() % (_a - 19)); // 10..(_a - 10)
        _answer = _a - _b;
    }

    // Build four unique choices including the answer, then shuffle.
    static const int deltas[] = {-1, 1, -2, 2, -3, 3, -4, 4, -5, 5, -10, 10};
    static const int NDELTA = (int)(sizeof(deltas) / sizeof(deltas[0]));
    _vals[0] = _answer;
    int n = 1;
    while (n < CHOICE_COUNT) {
        int v = _answer + deltas[(int)(esp_random() % NDELTA)];
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

bool MathQuizApp::run(void)
{
    ESP_UTILS_LOGI("mathquiz run");

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

bool MathQuizApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void MathQuizApp::onChoicePress(lv_event_t *e)
{
    if (_instance == nullptr) {
        return;
    }
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    _instance->handleChoice(idx);
}

void MathQuizApp::handleChoice(int idx)
{
    if (_phase != Phase::Playing || idx < 0 || idx >= CHOICE_COUNT) {
        return;
    }
    if (idx == _correct) {
        // Correct: green hold, score up, then the next problem.
        setChoiceColor(idx, COL_OK);
        _score++;
        updateScore();
        _wrong_pad = -1;
        _phase = Phase::Feedback;
        _feedback_until_us = now_us() + FEEDBACK_MS * 1000;
    } else {
        // Wrong: gentle warm-orange flash on that choice; just try again.
        setChoiceColor(idx, COL_BAD);
        _wrong_pad = idx;
        _wrong_until_us = now_us() + WRONG_FLASH_MS * 1000;
    }
}

void MathQuizApp::onTick(lv_timer_t *timer)
{
    MathQuizApp *self = static_cast<MathQuizApp *>(lv_timer_get_user_data(timer));
    if (self) {
        self->tick();
    }
}

void MathQuizApp::tick(void)
{
    uint32_t now = now_us();

    // Expire a wrong-tap flash, restoring that choice to idle.
    if (_wrong_pad >= 0 && now >= _wrong_until_us) {
        setChoiceColor(_wrong_pad, COL_CHOICE);
        _wrong_pad = -1;
    }

    // After the correct-answer green hold, move on to the next problem.
    if (_phase == Phase::Feedback && now >= _feedback_until_us) {
        nextProblem();
        _phase = Phase::Playing;
    }
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, MathQuizApp, APP_NAME, []()
{
    return std::shared_ptr<MathQuizApp>(MathQuizApp::requestInstance(), [](MathQuizApp *) {});
})

} // namespace esp_brookesia::apps
