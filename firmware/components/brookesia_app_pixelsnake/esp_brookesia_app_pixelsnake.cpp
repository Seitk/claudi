/*
 * Pixel Snake game — see header. Round 466x466 AMOLED, steer by dragging.
 *
 * Flow: Playing -> GameOver (tap anywhere to replay). A 15x15 grid of 28px
 * square cells is centered on the round glass. The snake auto-advances one cell
 * every step (~140ms, easing toward ~80ms as it grows). Each step we look at the
 * latest touch point: dx/dy from the head's pixel center pick the dominant axis,
 * and we turn that way unless it would be a direct 180 reversal. Eating the red
 * food grows the snake by one and respawns food at a random empty cell. Running
 * into a wall or your own body ends the round; the best score persists (a
 * file-scope static) across launches.
 *
 * All gameplay elements are crisp SQUARE blocks (radius 0, no border): a recycled
 * pool of body blocks plus one food block, created once in run() and only
 * shown/hidden/repositioned per frame.
 */
#include "esp_brookesia_app_pixelsnake.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:pixelsnake"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"
#include "game_icons.h"

#include <cmath>
#include <cstring>

#define APP_NAME "Pixel Snake"

// --- Geometry (screen is 466x466, center 233,233) ---
#define SCREEN_HALF 233     // screen center; LV_ALIGN_CENTER offset = px - 233

// --- Step pacing (milliseconds) ---
#define STEP_MS_START   140     // step interval at the start
#define STEP_MS_FLOOR    80     // fastest step interval
#define STEP_MS_PER_LEN   3     // shave this many ms off per body cell grown

// --- Retro palette ---
#define COL_BG        0x07070C  // screen background
#define COL_BOARD     0x101018  // faint board fill behind the grid
#define COL_BODY      0x3CCB4A  // snake body green
#define COL_HEAD      0x8CFF9A  // brighter snake head
#define COL_FOOD      0xFF3B3B  // bright-red food
#define COL_TEXT      0xFFFFFF
#define COL_OVER      0xFF6B6B  // game-over accent

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

PixelSnakeApp *PixelSnakeApp::_instance = nullptr;

// Best score persists across launches (the only allowed persistent state).
static int s_best = 0;

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

// Style a fresh object as a crisp filled square block of the given color.
static void style_block(lv_obj_t *o, uint32_t color)
{
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

PixelSnakeApp *PixelSnakeApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new PixelSnakeApp();
    }
    return _instance;
}

PixelSnakeApp::PixelSnakeApp(): App(APP_NAME, &icon_pixelsnake, true, false, false) {}
PixelSnakeApp::~PixelSnakeApp() {}

bool PixelSnakeApp::run(void)
{
    ESP_UTILS_LOGI("pixelsnake run");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // Whole-screen touch drives both steering and the game-over restart. The back
    // gesture still works: Brookesia reads the touch device directly.
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, onPressed, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(scr, onPressing, LV_EVENT_PRESSING, this);

    // A faint square board behind the grid, centered on the screen.
    lv_obj_t *board = lv_obj_create(scr);
    lv_obj_set_size(board, GRID * CELL, GRID * CELL);
    style_block(board, COL_BOARD);
    lv_obj_align(board, LV_ALIGN_CENTER, 0, 0);

    // Recycled pool of body square blocks (created once, shown/hidden in render).
    for (int i = 0; i < POOL; ++i) {
        lv_obj_t *b = lv_obj_create(scr);
        lv_obj_set_size(b, CELL, CELL);
        style_block(b, COL_BODY);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        _blocks[i] = b;
    }

    // The single red food block.
    _food_block = lv_obj_create(scr);
    lv_obj_set_size(_food_block, CELL, CELL);
    style_block(_food_block, COL_FOOD);
    lv_obj_add_flag(_food_block, LV_OBJ_FLAG_HIDDEN);

    // Score (current length-based) small at top center.
    _score_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_score_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_score_label, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(_score_label, LV_ALIGN_TOP_MID, 0, 14);
    lv_label_set_text(_score_label, "0");

    // Centered game-over banner.
    _big = lv_label_create(scr);
    lv_obj_set_style_text_font(_big, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_big, lv_color_hex(COL_OVER), 0);
    lv_obj_set_style_text_align(_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_big, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(_big, "");

    resetGame();

    _timer = lv_timer_create(onTick, 16, this);
    return true;
}

bool PixelSnakeApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void PixelSnakeApp::resetGame(void)
{
    _phase = Phase::Playing;
    _score = 0;
    _step_ms = STEP_MS_START;
    _last_step_us = now_us();
    _have_touch = false;       // keep current dir until the player touches

    // Start length 3, moving right, centered: head at grid center, tail to its
    // left so the body trails behind the heading.
    int cx = GRID / 2;
    int cy = GRID / 2;
    _len = START_LEN;
    for (int i = 0; i < _len; ++i) {
        _sx[i] = cx - i;       // index 0 = head, higher indices trail left
        _sy[i] = cy;
    }
    _dir = Dir::Right;
    _next_dir = Dir::Right;

    spawnFood();

    lv_label_set_text(_big, "");
    lv_obj_add_flag(_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_score_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(_score_label, "%d", _score);

    render();
}

bool PixelSnakeApp::occupied(int gx, int gy) const
{
    for (int i = 0; i < _len; ++i) {
        if (_sx[i] == gx && _sy[i] == gy) {
            return true;
        }
    }
    return false;
}

// Place food at a uniformly-chosen empty cell. If the snake fills nearly the
// whole board, fall back to the first free cell scanned.
void PixelSnakeApp::spawnFood(void)
{
    int free_count = GRID * GRID - _len;
    if (free_count <= 0) {
        return;  // board full (a win, effectively); leave food where it is
    }
    int pick = (int)(esp_random() % (uint32_t)free_count);
    int seen = 0;
    for (int gy = 0; gy < GRID; ++gy) {
        for (int gx = 0; gx < GRID; ++gx) {
            if (occupied(gx, gy)) {
                continue;
            }
            if (seen == pick) {
                _food_x = gx;
                _food_y = gy;
                return;
            }
            seen++;
        }
    }
}

// Turn toward the latest touch point: dominant axis, never a 180 reversal.
void PixelSnakeApp::applySteer(void)
{
    if (!_have_touch) {
        return;  // no input yet — keep heading
    }
    int hx = cellCx(_sx[0]);
    int hy = cellCy(_sy[0]);
    int dx = _touch_x - hx;
    int dy = _touch_y - hy;

    Dir want = _dir;
    if (abs(dx) > abs(dy)) {
        want = (dx > 0) ? Dir::Right : Dir::Left;
    } else {
        want = (dy > 0) ? Dir::Down : Dir::Up;
    }

    // Reject a direct reversal against the *current* heading.
    bool reversal =
        (want == Dir::Left  && _dir == Dir::Right) ||
        (want == Dir::Right && _dir == Dir::Left)  ||
        (want == Dir::Up    && _dir == Dir::Down)  ||
        (want == Dir::Down  && _dir == Dir::Up);
    if (!reversal) {
        _next_dir = want;
    }
}

void PixelSnakeApp::step(void)
{
    applySteer();
    _dir = _next_dir;

    int nx = _sx[0];
    int ny = _sy[0];
    switch (_dir) {
        case Dir::Up:    ny -= 1; break;
        case Dir::Down:  ny += 1; break;
        case Dir::Left:  nx -= 1; break;
        case Dir::Right: nx += 1; break;
    }

    // Wall death.
    if (nx < 0 || nx >= GRID || ny < 0 || ny >= GRID) {
        enterGameOver();
        return;
    }

    bool eating = (nx == _food_x && ny == _food_y);

    // Self-collision: hitting any body cell. The tail cell will vacate this step
    // (unless we're eating and thus growing), so ignore the current tail.
    int body_limit = eating ? _len : (_len - 1);
    for (int i = 0; i < body_limit; ++i) {
        if (_sx[i] == nx && _sy[i] == ny) {
            enterGameOver();
            return;
        }
    }

    if (eating) {
        // Grow: shift the whole body back by one, then place the new head.
        if (_len < MAX_LEN) {
            _len += 1;
        }
        for (int i = _len - 1; i > 0; --i) {
            _sx[i] = _sx[i - 1];
            _sy[i] = _sy[i - 1];
        }
        _sx[0] = nx;
        _sy[0] = ny;

        _score = _len - START_LEN;
        if (_score > s_best) {
            s_best = _score;
        }
        lv_label_set_text_fmt(_score_label, "%d", _score);

        // Speed up gently as the snake grows (floored).
        uint32_t faster = STEP_MS_START - (uint32_t)(_score * STEP_MS_PER_LEN);
        _step_ms = (faster < STEP_MS_FLOOR) ? STEP_MS_FLOOR : faster;

        spawnFood();
    } else {
        // Move: shift body back by one, then set the new head.
        for (int i = _len - 1; i > 0; --i) {
            _sx[i] = _sx[i - 1];
            _sy[i] = _sy[i - 1];
        }
        _sx[0] = nx;
        _sy[0] = ny;
    }

    render();
}

// Sync the recycled block pool and food block to current game state.
void PixelSnakeApp::render(void)
{
    int shown = (_len < POOL) ? _len : POOL;
    for (int i = 0; i < POOL; ++i) {
        lv_obj_t *b = _blocks[i];
        if (i < shown) {
            // LV_ALIGN_CENTER centers the block, then offsets by (cell-center - 233).
            int px = cellCx(_sx[i]);
            int py = cellCy(_sy[i]);
            lv_obj_align(b, LV_ALIGN_CENTER, px - SCREEN_HALF, py - SCREEN_HALF);
            // Brighter head block; plain body for the rest.
            lv_obj_set_style_bg_color(b, lv_color_hex(i == 0 ? COL_HEAD : COL_BODY), 0);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        }
    }

    int fpx = cellCx(_food_x);
    int fpy = cellCy(_food_y);
    lv_obj_align(_food_block, LV_ALIGN_CENTER, fpx - SCREEN_HALF, fpy - SCREEN_HALF);
    lv_obj_clear_flag(_food_block, LV_OBJ_FLAG_HIDDEN);
}

void PixelSnakeApp::enterGameOver(void)
{
    _phase = Phase::GameOver;
    lv_label_set_text_fmt(_big, "GAME OVER\nScore %d\nBest %d\n\ntap to play again",
                          _score, s_best);
    lv_obj_clear_flag(_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_big);
}

void PixelSnakeApp::readTouch(void)
{
    lv_indev_t *in = lv_indev_active();
    if (!in) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    _touch_x = p.x;
    _touch_y = p.y;
    _have_touch = true;
}

void PixelSnakeApp::onPressed(lv_event_t *e)
{
    PixelSnakeApp *self = static_cast<PixelSnakeApp *>(lv_event_get_user_data(e));
    if (!self) {
        return;
    }
    if (self->_phase == Phase::GameOver) {
        self->resetGame();   // a tap anywhere on the banner replays
        return;
    }
    self->readTouch();
}

void PixelSnakeApp::onPressing(lv_event_t *e)
{
    PixelSnakeApp *self = static_cast<PixelSnakeApp *>(lv_event_get_user_data(e));
    if (!self || self->_phase != Phase::Playing) {
        return;
    }
    self->readTouch();
}

void PixelSnakeApp::onTick(lv_timer_t *timer)
{
    PixelSnakeApp *self = static_cast<PixelSnakeApp *>(lv_timer_get_user_data(timer));
    if (self) {
        self->tick();
    }
}

void PixelSnakeApp::tick(void)
{
    if (_phase != Phase::Playing) {
        return;
    }
    uint32_t now = now_us();
    // Step whenever the interval has elapsed (16ms tick counts the time).
    if ((now - _last_step_us) >= _step_ms * 1000) {
        _last_step_us = now;
        step();
    }
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, PixelSnakeApp, APP_NAME, []()
{
    return std::shared_ptr<PixelSnakeApp>(PixelSnakeApp::requestInstance(), [](PixelSnakeApp *) {});
})

} // namespace esp_brookesia::apps
