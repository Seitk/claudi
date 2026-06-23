/*
 * Pixel 2048 — see header. Round 466x466 AMOLED, swipe to slide.
 *
 * Flow: Playing -> (board full, no merges) -> GameOver -> tap to restart.
 *
 * The board is a flat 4x4 int array (row-major, 0 == empty). A swipe resolves to one
 * of four directions and runs the classic 2048 move: every line is compacted toward
 * the swipe edge, equal neighbours merge once into their sum (scoring the sum), and
 * the result is written back. If the move changed anything, a fresh 2 (90%) or 4
 * (10%) is dropped into a random empty cell and the grid is repainted.
 *
 * Rendering is a crisp retro grid: 16 square tile blocks are created once in run()
 * and recycled — each move just recolours each block and rewrites its number label.
 */
#include "esp_brookesia_app_pixel2048.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:px2048"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"
#include "game_icons.h"

#include <cmath>
#include <cstring>

#define APP_NAME "Pixel 2048"

// --- Geometry (screen is 466x466, center 233,233, visible radius ~233) ---
#define SCREEN_CX 233
#define SCREEN_CY 233
#define TILE       62                              // tile side, px
#define GAP         8                              // gap between tiles, px
#define BOARD_PX    (4 * TILE + 3 * GAP)           // 4*62 + 3*8 = 272 total
#define BOARD_ORIGIN ((466 - BOARD_PX) / 2)        // top-left of the grid: 97

// Swipe must travel at least this far (px) to count as a flick.
#define SWIPE_MIN  32

#define COL_BG       0x12121A
#define COL_EMPTY    0x2A2A3A   // empty-cell "slot" colour
#define COL_SCORE    0xFFFFFF
#define COL_BANNER   0xEDC22E   // gold "2048!" flash
#define COL_OVER     0xFFFFFF
#define COL_TEXT_DK  0x2B2B33   // dark number text (small values)
#define COL_TEXT_LT  0xFFFFFF   // light number text (>=128)

#define BANNER_MS  1600   // how long the one-shot "2048!" flash lingers

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

Pixel2048App *Pixel2048App::_instance = nullptr;

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

Pixel2048App *Pixel2048App::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new Pixel2048App();
    }
    return _instance;
}

Pixel2048App::Pixel2048App(): App(APP_NAME, &icon_pixel2048, true, false, false) {}
Pixel2048App::~Pixel2048App() {}

// Warm 2048 palette keyed by tile value; unknown/large values reuse 2048's gold.
uint32_t Pixel2048App::tileColor(int value)
{
    switch (value) {
    case 2:    return 0xEEE4DA;
    case 4:    return 0xEDE0C8;
    case 8:    return 0xF2B179;
    case 16:   return 0xF59563;
    case 32:   return 0xF67C5F;
    case 64:   return 0xF65E3B;
    case 128:  return 0xEDCF72;
    case 256:  return 0xEDCC61;
    case 512:  return 0xEDC850;
    case 1024: return 0xEDC53F;
    case 2048: return 0xEDC22E;
    default:   return 0xEDC22E;  // beyond 2048, keep the top gold
    }
}

bool Pixel2048App::run(void)
{
    ESP_UTILS_LOGI("pixel2048 run");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    // The whole screen is the swipe surface: PRESSED records the flick start and
    // RELEASED resolves a direction. PRESS_LOST is treated like a release so a drag
    // off-screen still finishes the flick. The back gesture still works: Brookesia
    // reads the touch device directly, independent of LVGL click routing.
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, onPressed, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(scr, onReleased, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(scr, onReleased, LV_EVENT_PRESS_LOST, this);

    // Build the 16 tile blocks once; each move only recolours / relabels them.
    for (int i = 0; i < CELLS; ++i) {
        int r = i / N;
        int c = i % N;
        int x = BOARD_ORIGIN + c * (TILE + GAP);
        int y = BOARD_ORIGIN + r * (TILE + GAP);

        lv_obj_t *t = lv_obj_create(scr);
        lv_obj_set_size(t, TILE, TILE);
        lv_obj_set_style_radius(t, 0, 0);          // crisp square, no rounding
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(COL_EMPTY), 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);   // taps pass to the screen
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        // Position by top-left using a center-aligned offset (coord - center).
        lv_obj_align(t, LV_ALIGN_CENTER,
                     x + TILE / 2 - SCREEN_CX,
                     y + TILE / 2 - SCREEN_CY);
        _tiles[i] = t;

        lv_obj_t *lbl = lv_label_create(t);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_DK), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        lv_label_set_text(lbl, "");
        _tileLabels[i] = lbl;
    }

    // Score, top center, above the grid.
    _score_label = lv_label_create(scr);
    lv_obj_set_style_text_font(_score_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_score_label, lv_color_hex(COL_SCORE), 0);
    lv_obj_set_style_text_align(_score_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_score_label, LV_ALIGN_TOP_MID, 0, 40);
    lv_label_set_text(_score_label, "0");

    // One-shot "2048!" flash, just under the score.
    _banner = lv_label_create(scr);
    lv_obj_set_style_text_font(_banner, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_banner, lv_color_hex(COL_BANNER), 0);
    lv_obj_set_style_text_align(_banner, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_banner, LV_ALIGN_TOP_MID, 0, 74);
    lv_label_set_text(_banner, "");

    // Game-over overlay banner, centered over the grid.
    _over = lv_label_create(scr);
    lv_obj_set_style_text_font(_over, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(_over, lv_color_hex(COL_OVER), 0);
    lv_obj_set_style_text_align(_over, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_over, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(_over, "");
    lv_obj_add_flag(_over, LV_OBJ_FLAG_HIDDEN);

    resetGame();

    // Only used to time out the transient "2048!" banner.
    _timer = lv_timer_create(onTick, 30, this);
    return true;
}

bool Pixel2048App::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void Pixel2048App::resetGame(void)
{
    _phase = Phase::Playing;
    _score = 0;
    _won = false;
    _banner_until_us = 0;

    memset(_board, 0, sizeof(_board));
    // Two starting tiles, like the classic game.
    spawnTile();
    spawnTile();

    lv_label_set_text(_score_label, "0");
    lv_label_set_text(_banner, "");
    lv_obj_add_flag(_over, LV_OBJ_FLAG_HIDDEN);

    render();
}

// Paint every block from the board: empty cells get the dark slot colour and no
// text; filled cells get their warm palette colour and number (light text >=128).
void Pixel2048App::render(void)
{
    for (int i = 0; i < CELLS; ++i) {
        int v = _board[i];
        if (v == 0) {
            lv_obj_set_style_bg_color(_tiles[i], lv_color_hex(COL_EMPTY), 0);
            lv_label_set_text(_tileLabels[i], "");
        } else {
            lv_obj_set_style_bg_color(_tiles[i], lv_color_hex(tileColor(v)), 0);
            uint32_t txt = (v >= 128) ? COL_TEXT_LT : COL_TEXT_DK;
            lv_obj_set_style_text_color(_tileLabels[i], lv_color_hex(txt), 0);
            lv_label_set_text_fmt(_tileLabels[i], "%d", v);
        }
    }
    lv_label_set_text_fmt(_score_label, "%d", _score);
}

// Classic 2048 move. We walk each of the 4 lines in the direction of travel by
// reading cells via a (start, step) addressing scheme, compact non-zero values,
// merge equal neighbours once, then write the line back. Returns true if anything
// moved or merged. Merged sums are added to the score; reaching 2048 sets _won.
bool Pixel2048App::doMove(Dir dir)
{
    // For each of the N lines, `base` is the index of the line's first cell in the
    // travel direction and `step` advances along that line.
    //  Left : rows, scan left->right    (base = r*N,         step = +1)
    //  Right: rows, scan right->left     (base = r*N + N-1,   step = -1)
    //  Up   : cols, scan top->bottom     (base = c,           step = +N)
    //  Down : cols, scan bottom->top     (base = c + N*(N-1), step = -N)
    bool changed = false;

    for (int line = 0; line < N; ++line) {
        int base, step;
        switch (dir) {
        case Dir::Left:  base = line * N;             step = 1;   break;
        case Dir::Right: base = line * N + (N - 1);   step = -1;  break;
        case Dir::Up:    base = line;                 step = N;   break;
        case Dir::Down:  base = line + N * (N - 1);   step = -N;  break;
        default:         base = 0;                    step = 1;   break;
        }

        // Pull the line out in travel order (index 0 == leading edge).
        int in[N];
        for (int k = 0; k < N; ++k) {
            in[k] = _board[base + step * k];
        }

        // Compact non-zero values toward the leading edge, then merge equal
        // adjacent pairs once (each tile can be part of at most one merge).
        int out[N] = { 0, 0, 0, 0 };
        int w = 0;       // write cursor in `out`
        int last = -1;   // value awaiting a possible merge, or -1
        for (int k = 0; k < N; ++k) {
            int v = in[k];
            if (v == 0) {
                continue;
            }
            if (last == v) {
                // Merge into the previously placed tile.
                out[w - 1] = v * 2;
                _score += v * 2;
                if (v * 2 == 2048) {
                    _won = true;
                }
                last = -1;  // this slot is now closed to further merges
            } else {
                out[w] = v;
                last = v;
                ++w;
            }
        }

        // Write the line back; flag a change if any cell differs.
        for (int k = 0; k < N; ++k) {
            int idx = base + step * k;
            if (_board[idx] != out[k]) {
                changed = true;
            }
            _board[idx] = out[k];
        }
    }

    return changed;
}

// Drop a fresh tile (2 with 90%, 4 with 10%) into a uniformly random empty cell.
void Pixel2048App::spawnTile(void)
{
    int empties[CELLS];
    int n = 0;
    for (int i = 0; i < CELLS; ++i) {
        if (_board[i] == 0) {
            empties[n++] = i;
        }
    }
    if (n == 0) {
        return;
    }
    int pick = empties[esp_random() % (uint32_t)n];
    _board[pick] = ((esp_random() % 10) == 0) ? 4 : 2;  // 10% -> 4, else 2
}

// A move is possible if any cell is empty, or any pair of horizontal/vertical
// neighbours hold the same value (and so could merge).
bool Pixel2048App::hasMove(void) const
{
    for (int i = 0; i < CELLS; ++i) {
        if (_board[i] == 0) {
            return true;
        }
    }
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            int v = _board[r * N + c];
            if (c + 1 < N && _board[r * N + (c + 1)] == v) {
                return true;
            }
            if (r + 1 < N && _board[(r + 1) * N + c] == v) {
                return true;
            }
        }
    }
    return false;
}

void Pixel2048App::enterGameOver(void)
{
    _phase = Phase::GameOver;
    lv_label_set_text(_banner, "");
    _banner_until_us = 0;
    lv_label_set_text_fmt(_over, "Game Over\nScore %d\n\ntap to restart", _score);
    lv_obj_clear_flag(_over, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_over);
}

void Pixel2048App::onPressed(lv_event_t *e)
{
    Pixel2048App *self = static_cast<Pixel2048App *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    lv_indev_t *in = lv_indev_active();
    if (in == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    self->handlePressed(p.x, p.y);
}

void Pixel2048App::onReleased(lv_event_t *e)
{
    Pixel2048App *self = static_cast<Pixel2048App *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    lv_indev_t *in = lv_indev_active();
    if (in == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    self->handleReleased(p.x, p.y);
}

// Remember the flick start; on the game-over screen a tap restarts instead.
void Pixel2048App::handlePressed(int x, int y)
{
    if (_phase == Phase::GameOver) {
        resetGame();
        return;
    }
    _press_x = x;
    _press_y = y;
}

// Resolve the press-start vs release delta into a direction and play the move.
void Pixel2048App::handleReleased(int x, int y)
{
    if (_phase != Phase::Playing) {
        return;
    }
    int dx = x - _press_x;
    int dy = y - _press_y;
    int adx = (dx < 0) ? -dx : dx;
    int ady = (dy < 0) ? -dy : dy;

    // Too short to be a deliberate flick: ignore.
    if (adx < SWIPE_MIN && ady < SWIPE_MIN) {
        return;
    }

    Dir dir;
    if (adx >= ady) {
        dir = (dx > 0) ? Dir::Right : Dir::Left;
    } else {
        dir = (dy > 0) ? Dir::Down : Dir::Up;
    }

    bool was_won = _won;
    if (doMove(dir)) {
        spawnTile();
        render();
        // First time we hit 2048: flash the one-shot banner (play continues).
        if (_won && !was_won) {
            lv_label_set_text(_banner, "2048!");
            _banner_until_us = now_us() + BANNER_MS * 1000;
        }
        // If the new board is locked, end the game.
        if (!hasMove()) {
            enterGameOver();
        }
    }
}

void Pixel2048App::onTick(lv_timer_t *timer)
{
    Pixel2048App *self = static_cast<Pixel2048App *>(lv_timer_get_user_data(timer));
    if (self) {
        self->tick();
    }
}

void Pixel2048App::tick(void)
{
    // The only transient timing: clear the one-shot "2048!" flash when it expires.
    if (_banner_until_us != 0 && now_us() >= _banner_until_us) {
        lv_label_set_text(_banner, "");
        _banner_until_us = 0;
    }
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, Pixel2048App, APP_NAME, []()
{
    return std::shared_ptr<Pixel2048App>(Pixel2048App::requestInstance(), [](Pixel2048App *) {});
})

} // namespace esp_brookesia::apps
