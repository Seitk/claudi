/*
 * Math — a gentle 2-digit add & subtract quiz for grade-1 kids, shipped as an
 * ESP-Brookesia phone app (self-registers into the launcher). Responsive: lays
 * out from the live display size, so it works on both the round AMOLED and the
 * rectangular watch.
 *
 * A problem shows at the top ("37 + 12 = ?"); four answer choices sit in a 2x2
 * grid. Tap the right one: it flashes green, the score ticks up, and the next
 * problem appears. A wrong tap is gentle — that choice flashes warm-orange and
 * you simply try again (no penalty, no game over). Addition keeps the sum <= 99;
 * subtraction never goes negative; both operands are always two digits.
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class MathQuizApp: public systems::phone::App {
public:
    static MathQuizApp *requestInstance();
    ~MathQuizApp();

protected:
    MathQuizApp();

    // Build the UI on the default screen (lv_scr_act()).
    bool run(void) override;
    // Back gesture: leave the game, return to launcher.
    bool back(void) override;

private:
    static MathQuizApp *_instance;

    static constexpr int CHOICE_COUNT = 4;

    enum class Phase { Playing, Feedback };

    // ~30ms loop: expires the wrong-tap flash and the correct-tap feedback hold.
    static void onTick(lv_timer_t *timer);
    void tick(void);

    // A choice was tapped; the choice index 0..3 is the event user data.
    static void onChoicePress(lv_event_t *e);
    void handleChoice(int idx);

    void nextProblem(void);                 // new operands + shuffled choices
    void setChoiceColor(int idx, uint32_t bg);
    void updateScore(void);

    // Widgets (created in run(); recycled by the core on app close).
    lv_obj_t *_score_label = nullptr;
    lv_obj_t *_problem_label = nullptr;
    lv_obj_t *_choice[CHOICE_COUNT] = {nullptr};
    lv_obj_t *_choice_label[CHOICE_COUNT] = {nullptr};
    lv_timer_t *_timer = nullptr;

    // Problem state.
    Phase _phase = Phase::Playing;
    int _a = 0, _b = 0;          // the two operands (always 2-digit)
    char _op = '+';              // '+' or '-'
    int _answer = 0;             // correct result
    int _vals[CHOICE_COUNT] = {0};  // the four shown choices (one is _answer)
    int _correct = 0;            // index into _vals/_choice of the right answer
    int _score = 0;              // running count of correct answers

    // Feedback timing (esp_timer microseconds).
    int _wrong_pad = -1;         // a choice currently flashing warm-orange
    uint32_t _wrong_until_us = 0;
    uint32_t _feedback_until_us = 0;  // green hold before the next problem
};

} // namespace esp_brookesia::apps
