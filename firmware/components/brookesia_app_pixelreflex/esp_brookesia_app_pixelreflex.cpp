/*
 * Pixel Reflex game — see header. Round 466x466 AMOLED.
 *
 * Flow: a 3x3 grid of chunky square pixel cells fills the screen center; all cells
 * are dim except one bright target. Tap the lit cell to score +1, see your reaction
 * time in ms, and immediately get a new (never-repeating) target while the timeout
 * tightens from ~1500ms toward ~550ms as the score climbs. Tapping a dim cell is
 * ignored (forgiving). A big score sits top-center, the last reaction time just
 * below, and a thin time-bar near the bottom shrinks to show the remaining time for
 * the current target. If the lit cell's timeout expires before it's tapped it's
 * GAME OVER: the final score, the best score (persists across launches), and a
 * "tap to retry" prompt show; a tap anywhere starts a fresh game.
 */
#include "esp_brookesia_app_pixelreflex.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:pixelreflex"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"
#include "game_icons.h"

#include <cstring>

#define APP_NAME "Pixel Reflex"

// --- Geometry (screen is 466x466, center 233,233, visible radius ~233) ---
#define SCREEN_CX 233
#define SCREEN_CY 233
// 3 cells + 2 gaps span ~300px and stay well within the round bezel.
#define CELL_SIZE   92      // chunky square pixel cell
#define CELL_GAP    10      // gap between cells
#define GRID_SPAN   (CELL_SIZE * 3 + CELL_GAP * 2)  // = 296px total
// Top-left of the grid relative to the screen center (so it stays centered).
#define GRID_ORIGIN (-(GRID_SPAN / 2))  // = -148

// --- Pacing (milliseconds) ---
#define TIMEOUT_START_MS  1500   // generous first target
#define TIMEOUT_END_MS     550   // floor as the game speeds up
#define TIMEOUT_STEP_MS     45   // how much the timeout tightens per hit

// --- Retro palette ---
#define COL_BG     0x07070C
#define COL_DIM    0x2A2A3A   // dim (unlit) cell color
#define COL_TEXT   0xFFFFFF
#define COL_RT     0x9DE0FF   // soft blue reaction-time text
#define COL_BAR    0xFFD93B   // sunny yellow time-bar
#define COL_BAR_BG 0x1A1A22   // dark time-bar track

// Bright target colors, cycled per lit cell (red/green/blue/yellow/purple/orange).
static const uint32_t LIT_COLORS[] = {
    0xFF3B3B, 0x3BFF6B, 0x3B9DFF, 0xFFE03B, 0xB36BFF, 0xFF9F3B,
};
#define LIT_COLOR_COUNT (sizeof(LIT_COLORS) / sizeof(LIT_COLORS[0]))

// Time-bar dimensions (a plain rectangle whose width we set each tick).
#define BAR_W_MAX 280
#define BAR_H     14

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

PixelReflexApp *PixelReflexApp::_instance = nullptr;

// Best score persists across launches (the only allowed cross-launch state).
static int s_best = 0;

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

PixelReflexApp *PixelReflexApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PixelReflexApp();
    }
    return _instance;
}

PixelReflexApp::PixelReflexApp(): App(APP_NAME, &icon_pixelreflex, true, false, false) {}
PixelReflexApp::~PixelReflexApp() {}

bool PixelReflexApp::run(void)
{
    ESP_UTILS_LOGI("pixelreflex run");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // The whole screen is a press target used only to restart on the game-over
    // screen. The back gesture still works: Brookesia reads the touch device
    // directly, independent of LVGL click routing.
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, onScreenPress, LV_EVENT_PRESSED, this);

    // Build the 3x3 grid of chunky square pixel cells once; we only recolor and
    // show/hide them after this. Crisp squares: radius 0, no border, no padding.
    for (int i = 0; i < CELLS; ++i) {
        int row = i / GRID;
        int col = i % GRID;
        int x = GRID_ORIGIN + col * (CELL_SIZE + CELL_GAP);
        int y = GRID_ORIGIN + row * (CELL_SIZE + CELL_GAP);

        lv_obj_t *c = lv_obj_create(scr);
        lv_obj_set_size(c, CELL_SIZE, CELL_SIZE);
        lv_obj_set_style_radius(c, 0, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c, lv_color_hex(COL_DIM), 0);
        // Each cell is its own tap target (like tappop's poppers).
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(c, LV_ALIGN_CENTER, x, y);
        lv_obj_add_event_cb(c, onCellClick, LV_EVENT_CLICKED, this);
        _cells[i] = c;
    }

    // Score, big at top center.
    _score_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_score_label, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(_score_label, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(_score_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_score_label, LV_ALIGN_TOP_MID, 0, 30);
    lv_label_set_text(_score_label, "0");

    // Reaction time "123 ms", small, just under the score.
    _rt_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_rt_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_rt_label, lv_color_hex(COL_RT), 0);
    lv_obj_set_style_text_align(_rt_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_rt_label, LV_ALIGN_TOP_MID, 0, 84);
    lv_label_set_text(_rt_label, "");

    // Thin shrinking time-bar near the bottom (a plain rectangle; width set per
    // tick). Square corners to keep the pixel look.
    _timebar = lv_obj_create(scr);
    lv_obj_set_size(_timebar, BAR_W_MAX, BAR_H);
    lv_obj_set_style_radius(_timebar, 0, 0);
    lv_obj_set_style_border_width(_timebar, 0, 0);
    lv_obj_set_style_pad_all(_timebar, 0, 0);
    lv_obj_set_style_bg_opa(_timebar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(_timebar, lv_color_hex(COL_BAR), 0);
    lv_obj_clear_flag(_timebar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_timebar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(_timebar, LV_ALIGN_BOTTOM_MID, 0, -36);

    // Centered game-over summary banner.
    _over_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_over_label, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(_over_label, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(_over_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_over_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(_over_label, "");

    resetGame();

    _timer = lv_timer_create(onTick, 30, this);
    return true;
}

bool PixelReflexApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void PixelReflexApp::resetGame(void)
{
    _phase = Phase::Playing;
    _score = 0;
    _prev_lit = -1;
    _lit = -1;

    lv_label_set_text(_score_label, "0");
    lv_label_set_text(_rt_label, "");
    lv_label_set_text(_over_label, "");
    lv_obj_clear_flag(_score_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_rt_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_timebar, LV_OBJ_FLAG_HIDDEN);

    // All cells start dim; the grid is visible during play.
    for (int i = 0; i < CELLS; ++i) {
        setCellLit(i, false);
        lv_obj_clear_flag(_cells[i], LV_OBJ_FLAG_HIDDEN);
    }

    lightNewCell();
}

// Timeout for the current target: starts generous and tightens with the score,
// clamped at a snappy floor.
uint32_t PixelReflexApp::currentTimeoutMs(void) const
{
    uint32_t shrink = (uint32_t)_score * TIMEOUT_STEP_MS;
    if (shrink >= TIMEOUT_START_MS - TIMEOUT_END_MS) {
        return TIMEOUT_END_MS;
    }
    return TIMEOUT_START_MS - shrink;
}

// Recolor a cell bright (its target color) or dim.
void PixelReflexApp::setCellLit(int idx, bool lit)
{
    if (idx < 0 || idx >= CELLS) {
        return;
    }
    uint32_t color = lit ? _lit_color : COL_DIM;
    lv_obj_set_style_bg_color(_cells[idx], lv_color_hex(color), 0);
}

// Pick a fresh random cell (never the same one twice in a row), color it from the
// bright palette, light it, and start its countdown.
void PixelReflexApp::lightNewCell(void)
{
    // Dim the previous target, if any.
    if (_lit >= 0) {
        setCellLit(_lit, false);
    }

    // Choose a new index different from the last one.
    int idx;
    do {
        idx = (int)(esp_random() % (uint32_t)CELLS);
    } while (idx == _prev_lit && CELLS > 1);

    // Cycle through the bright palette so each target feels distinct.
    static uint32_t lit_count = 0;
    _lit_color = LIT_COLORS[lit_count % LIT_COLOR_COUNT];
    lit_count++;

    _lit = idx;
    _prev_lit = idx;
    _lit_start_us = now_us();
    _lit_timeout_ms = currentTimeoutMs();
    setCellLit(idx, true);

    // Reset the time-bar to full for the new target.
    lv_obj_set_size(_timebar, BAR_W_MAX, BAR_H);
}

void PixelReflexApp::enterGameOver(void)
{
    _phase = Phase::GameOver;

    if (_score > s_best) {
        s_best = _score;
    }

    // Hide the grid and the transient HUD.
    for (int i = 0; i < CELLS; ++i) {
        lv_obj_add_flag(_cells[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(_timebar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_score_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_rt_label, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text_fmt(_over_label, "Score %d\nBest %d\n\ntap to retry", _score, s_best);
}

void PixelReflexApp::onCellClick(lv_event_t *e)
{
    PixelReflexApp *self = static_cast<PixelReflexApp *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    if (self->_phase != Phase::Playing) {
        return;  // ignore stray clicks once the game has ended
    }
    lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
    // Only a tap on the currently-lit cell counts; dim cells are forgiven.
    if (self->_lit < 0 || self->_cells[self->_lit] != target) {
        return;
    }

    // Hit! Score, show the reaction time, then light a fresh target.
    uint32_t rt_ms = (now_us() - self->_lit_start_us) / 1000;
    self->_score += 1;
    lv_label_set_text_fmt(self->_score_label, "%d", self->_score);
    lv_label_set_text_fmt(self->_rt_label, "%lu ms", (unsigned long)rt_ms);

    self->lightNewCell();
}

void PixelReflexApp::onScreenPress(lv_event_t *e)
{
    PixelReflexApp *self = static_cast<PixelReflexApp *>(lv_event_get_user_data(e));
    if (self) {
        self->handleScreenPress();
    }
}

void PixelReflexApp::handleScreenPress(void)
{
    // During play, screen taps do nothing here (cell clicks handle scoring); on the
    // game-over screen, a tap anywhere starts a fresh game.
    if (_phase == Phase::GameOver) {
        resetGame();
    }
}

void PixelReflexApp::onTick(lv_timer_t *timer)
{
    PixelReflexApp *self = static_cast<PixelReflexApp *>(lv_timer_get_user_data(timer));
    if (self) {
        self->tick();
    }
}

void PixelReflexApp::tick(void)
{
    if (_phase != Phase::Playing || _lit < 0) {
        return;
    }

    uint32_t now = now_us();
    uint32_t elapsed_ms = (now - _lit_start_us) / 1000;

    // Timeout expired before the target was tapped -> game over.
    if (elapsed_ms >= _lit_timeout_ms) {
        enterGameOver();
        return;
    }

    // Shrink the time-bar to show remaining time for the current target.
    uint32_t remain_ms = _lit_timeout_ms - elapsed_ms;
    int w = (int)((uint32_t)BAR_W_MAX * remain_ms / _lit_timeout_ms);
    if (w < 1) {
        w = 1;
    }
    lv_obj_set_size(_timebar, w, BAR_H);
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PixelReflexApp, APP_NAME, []()
{
    return std::shared_ptr<PixelReflexApp>(PixelReflexApp::requestInstance(), [](PixelReflexApp *) {});
})

} // namespace esp_brookesia::apps
