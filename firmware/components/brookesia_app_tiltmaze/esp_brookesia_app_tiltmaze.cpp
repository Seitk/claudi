/*
 * Tilt Maze game — see header. Round 466x466 AMOLED, tilt the board to roll.
 *
 * Flow: load level 0 -> roll the ball to the goal -> brief "Level done!" flash
 * -> load the next level (wraps back to 0). No fail state, so it stays relaxed.
 *
 * Each ~16ms tick reads the accelerometer (in g), nudges the ball's velocity by
 * the tilt, applies friction, clamps speed, then moves X and Y separately and
 * resolves each against the wall-cell AABBs (the ball is itself treated as an
 * AABB). On a collision the ball is pushed back to the wall edge and that axis'
 * velocity is zeroed, which keeps it from tunneling through thin walls.
 *
 * Levels are fixed 10x10 char maps ('#' wall, '.' floor, 'S' start, 'G' goal);
 * the outer ring is always wall. Walls are drawn from a recycled pool of square
 * blocks created once in run() — only the current level's wall cells are shown.
 *
 * If the IMU isn't present, a centered message shows and physics is skipped so
 * the back gesture still works.
 */
#include "esp_brookesia_app_tiltmaze.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:tiltmaze"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"
#include "game_icons.h"
#include "claudi_imu.h"

#include <cmath>
#include <cstring>

#define APP_NAME "Tilt Maze"

// Maze grid dimensions. File-scope macros (defined AFTER all #includes, so they
// can't collide with any header token) so both the file-scope LEVELS table and
// the cell<->pixel helpers below can use them. 10x10 cells of 30px = 300px.
#define GRID 10
#define CELL 30

// --- Geometry (screen is 466x466, center 233,233, visible radius ~233) ---
#define SCREEN_CX 233
#define SCREEN_CY 233
// 10x10 grid of 30px cells = 300px; the grid origin (top-left of cell 0,0) is
// at (GRID_X0, GRID_Y0) so the 300px board is centered on the screen.
#define GRID_PX   (GRID * CELL)               // 300
#define GRID_X0   (SCREEN_CX - GRID_PX / 2)    // 83
#define GRID_Y0   (SCREEN_CY - GRID_PX / 2)    // 83
#define BALL_SZ   20                            // ball block is 20x20px

// --- Physics tuning (per 16ms tick). MAPPING IS UNVERIFIED on real hardware,
// --- and the X/Y axes or their signs may need flipping once tested. ---
#define AX_GAIN  60.0f
#define AY_GAIN  60.0f   // +ax -> ball accelerates +x (right), +ay -> +y (down)
#define FRICTION 0.90f   // velocity retained each tick
#define MAX_SPEED 8.0f   // px per tick cap, so the ball stays controllable
#define FLASH_MS 900     // how long the "Level done!" banner lingers

// --- Retro palette ---
#define COL_BG    0x07070C
#define COL_WALL  0x3A7BFF   // bright blue wall blocks
#define COL_BALL  0xFFE03B   // bright yellow ball
#define COL_GOAL  0x4CD964   // green goal
#define COL_TEXT  0xFFFFFF
#define COL_FLASH 0xFFD93B   // sunny "Level done!" text

// --- Hand-designed levels: 10x10 char maps, outer ring all walls. ---
// '#' wall, '.' floor, 'S' ball start, 'G' goal.
static const char *const LEVELS[][GRID] = {
    {   // Level 1 — gentle introduction
        "##########",
        "#S.......#",
        "#.######.#",
        "#.#....#.#",
        "#.#.##.#.#",
        "#.#.##.#.#",
        "#.#....#.#",
        "#.####.#.#",
        "#......#G#",
        "##########",
    },
    {   // Level 2 — a winding switchback
        "##########",
        "#S.#.....#",
        "##.#.###.#",
        "#..#.#...#",
        "#.##.#.###",
        "#....#...#",
        "####.###.#",
        "#......#.#",
        "#.####.#G#",
        "##########",
    },
    {   // Level 3 — a tighter spiral toward the center goal
        "##########",
        "#S.......#",
        "#.######.#",
        "#.#....#.#",
        "#.#.##.#.#",
        "#.#.#G.#.#",
        "#.#.####.#",
        "#.#......#",
        "#.######.#",
        "##########",
    },
};
#define LEVEL_COUNT ((int)(sizeof(LEVELS) / sizeof(LEVELS[0])))

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

TiltMazeApp *TiltMazeApp::_instance = nullptr;

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

// Cell pixel top-left for a grid (col, row).
static inline int cell_px_x(int col) { return GRID_X0 + col * CELL; }
static inline int cell_px_y(int row) { return GRID_Y0 + row * CELL; }

TiltMazeApp *TiltMazeApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new TiltMazeApp();
    }
    return _instance;
}

TiltMazeApp::TiltMazeApp(): App(APP_NAME, &icon_tiltmaze, true, false, false) {}
TiltMazeApp::~TiltMazeApp() {}

// Configure one block widget as a crisp square: radius 0, no border/pad, opaque.
static void style_block(lv_obj_t *o, int size, uint32_t color)
{
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

bool TiltMazeApp::run(void)
{
    ESP_UTILS_LOGI("tiltmaze run");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Wall block pool: created once, recycled (show/hide + reposition) per level.
    for (int i = 0; i < MAX_WALLS; ++i) {
        _walls[i] = lv_obj_create(scr);
        style_block(_walls[i], CELL, COL_WALL);
        lv_obj_add_flag(_walls[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Goal block (positioned per level).
    _goal = lv_obj_create(scr);
    style_block(_goal, CELL, COL_GOAL);

    // The rolling ball.
    _ball = lv_obj_create(scr);
    style_block(_ball, BALL_SZ, COL_BALL);

    // HUD: "Level N" top center.
    _hud = lv_label_create(scr);
    lv_obj_set_style_text_font(_hud, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(_hud, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(_hud, LV_ALIGN_TOP_MID, 0, 18);
    lv_label_set_text(_hud, "Level 1");

    // "Level done!" celebration banner (hidden except during the flash).
    _flash = lv_label_create(scr);
    lv_obj_set_style_text_font(_flash, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(_flash, lv_color_hex(COL_FLASH), 0);
    lv_obj_set_style_text_align(_flash, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_flash, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(_flash, "Level done!");
    lv_obj_add_flag(_flash, LV_OBJ_FLAG_HIDDEN);

    // Fallback message shown only when the IMU isn't present.
    _nosensor = lv_label_create(scr);
    lv_obj_set_style_text_font(_nosensor, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(_nosensor, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_align(_nosensor, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(_nosensor, LV_ALIGN_CENTER, 0, 90);
    lv_label_set_text(_nosensor, "Tilt sensor\nunavailable");
    lv_obj_add_flag(_nosensor, LV_OBJ_FLAG_HIDDEN);

    // Check IMU availability once per launch; skip physics in tick if absent.
    _imu_ok = claudi_imu_available();
    if (!_imu_ok) {
        lv_obj_clear_flag(_nosensor, LV_OBJ_FLAG_HIDDEN);
    }

    resetGame();

    _timer = lv_timer_create(onTick, 16, this);
    return true;
}

bool TiltMazeApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

void TiltMazeApp::resetGame(void)
{
    _level = 0;
    _level_done = false;
    _flash_until_us = 0;
    _ax = 0.0f;
    _ay = 0.0f;
    loadLevel(_level);
}

// Build the wall blocks and place the ball/goal for the given level.
void TiltMazeApp::loadLevel(int index)
{
    const char *const *map = LEVELS[index];

    // Reset transient state for the new level.
    _vx = 0.0f;
    _vy = 0.0f;
    _level_done = false;
    _flash_until_us = 0;
    lv_obj_add_flag(_flash, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text_fmt(_hud, "Level %d", index + 1);

    // Walk the map: show a wall block for every '#', remember 'S'/'G' cells.
    int wi = 0;
    int start_col = 1, start_row = 1;
    _goal_col = GRID - 2;
    _goal_row = GRID - 2;
    for (int row = 0; row < GRID; ++row) {
        const char *line = map[row];
        for (int col = 0; col < GRID; ++col) {
            char c = line[col];
            if (c == '#') {
                if (wi < MAX_WALLS) {
                    lv_obj_t *w = _walls[wi++];
                    lv_obj_set_pos(w, cell_px_x(col), cell_px_y(row));
                    lv_obj_clear_flag(w, LV_OBJ_FLAG_HIDDEN);
                }
            } else if (c == 'S') {
                start_col = col;
                start_row = row;
            } else if (c == 'G') {
                _goal_col = col;
                _goal_row = row;
            }
        }
    }
    _wall_count = wi;
    // Hide any pool blocks not used by this level.
    for (int i = wi; i < MAX_WALLS; ++i) {
        lv_obj_add_flag(_walls[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Place the goal block filling its cell.
    lv_obj_set_pos(_goal, cell_px_x(_goal_col), cell_px_y(_goal_row));

    // Center the ball within its start cell (cell is 30px, ball is 20px).
    _bx = (float)(cell_px_x(start_col) + (CELL - BALL_SZ) / 2);
    _by = (float)(cell_px_y(start_row) + (CELL - BALL_SZ) / 2);
    syncBallObj();
    lv_obj_move_foreground(_ball);
}

void TiltMazeApp::syncBallObj(void)
{
    lv_obj_set_pos(_ball, (int)lroundf(_bx), (int)lroundf(_by));
}

// AABB overlap test: does the ball at (x,y) (size BALL_SZ) hit any wall cell?
// Each wall cell occupies [cell_px, cell_px + CELL) on each axis.
bool TiltMazeApp::hitsWall(float x, float y) const
{
    const char *const *map = LEVELS[_level];
    float bx0 = x, by0 = y;
    float bx1 = x + BALL_SZ, by1 = y + BALL_SZ;

    // Only the grid cells the ball could touch need checking. Convert the ball
    // AABB to a span of grid columns/rows, clamped to the grid.
    int c0 = (int)floorf((bx0 - GRID_X0) / CELL);
    int c1 = (int)floorf((bx1 - 0.001f - GRID_X0) / CELL);
    int r0 = (int)floorf((by0 - GRID_Y0) / CELL);
    int r1 = (int)floorf((by1 - 0.001f - GRID_Y0) / CELL);
    if (c0 < 0) c0 = 0;
    if (r0 < 0) r0 = 0;
    if (c1 > GRID - 1) c1 = GRID - 1;
    if (r1 > GRID - 1) r1 = GRID - 1;

    for (int row = r0; row <= r1; ++row) {
        for (int col = c0; col <= c1; ++col) {
            if (map[row][col] != '#') {
                continue;
            }
            float wx0 = (float)cell_px_x(col);
            float wy0 = (float)cell_px_y(row);
            float wx1 = wx0 + CELL;
            float wy1 = wy0 + CELL;
            if (bx0 < wx1 && bx1 > wx0 && by0 < wy1 && by1 > wy0) {
                return true;
            }
        }
    }
    return false;
}

void TiltMazeApp::onTick(lv_timer_t *timer)
{
    TiltMazeApp *self = static_cast<TiltMazeApp *>(lv_timer_get_user_data(timer));
    if (self) {
        self->tick();
    }
}

void TiltMazeApp::tick(void)
{
    uint32_t now = now_us();

    // During the "Level done!" flash, just wait it out, then load the next level.
    if (_level_done) {
        if (now >= _flash_until_us) {
            _level = (_level + 1) % LEVEL_COUNT;  // wrap around
            loadLevel(_level);
        }
        return;
    }

    // No IMU: park the ball, skip physics (back gesture still exits).
    if (!_imu_ok) {
        return;
    }

    // Read tilt; if the read fails this tick, reuse the last good values.
    float ax, ay, az;
    if (claudi_imu_read_accel(&ax, &ay, &az)) {
        _ax = ax;
        _ay = ay;
        (void)az;
    }

    // Integrate velocity from tilt, then bleed it off with friction.
    _vx += AX_GAIN * _ax * 0.016f;
    _vy += AY_GAIN * _ay * 0.016f;
    _vx *= FRICTION;
    _vy *= FRICTION;

    // Clamp per-axis speed so the ball stays controllable at this tick rate.
    if (_vx > MAX_SPEED) _vx = MAX_SPEED;
    if (_vx < -MAX_SPEED) _vx = -MAX_SPEED;
    if (_vy > MAX_SPEED) _vy = MAX_SPEED;
    if (_vy < -MAX_SPEED) _vy = -MAX_SPEED;

    // Move X, then resolve X-vs-wall: if blocked, snap to the wall edge and stop
    // horizontal motion. Treating the ball as an AABB keeps this simple/robust.
    float nx = _bx + _vx;
    if (hitsWall(nx, _by)) {
        // Step the ball one pixel at a time toward nx until it would hit, so it
        // ends flush against the wall regardless of speed.
        float step = (_vx > 0.0f) ? 1.0f : -1.0f;
        float x = _bx;
        while (x != nx) {
            float tryx = x + step;
            if ((step > 0.0f && tryx > nx) || (step < 0.0f && tryx < nx)) {
                tryx = nx;
            }
            if (hitsWall(tryx, _by)) {
                break;
            }
            x = tryx;
            if (x == nx) {
                break;
            }
        }
        _bx = x;
        _vx = 0.0f;
    } else {
        _bx = nx;
    }

    // Move Y, then resolve Y-vs-wall the same way.
    float ny = _by + _vy;
    if (hitsWall(_bx, ny)) {
        float step = (_vy > 0.0f) ? 1.0f : -1.0f;
        float y = _by;
        while (y != ny) {
            float tryy = y + step;
            if ((step > 0.0f && tryy > ny) || (step < 0.0f && tryy < ny)) {
                tryy = ny;
            }
            if (hitsWall(_bx, tryy)) {
                break;
            }
            y = tryy;
            if (y == ny) {
                break;
            }
        }
        _by = y;
        _vy = 0.0f;
    } else {
        _by = ny;
    }

    syncBallObj();

    // Goal reached? Test the ball's AABB against the goal cell's AABB.
    float gx0 = (float)cell_px_x(_goal_col);
    float gy0 = (float)cell_px_y(_goal_row);
    float gx1 = gx0 + CELL;
    float gy1 = gy0 + CELL;
    if (_bx < gx1 && _bx + BALL_SZ > gx0 && _by < gy1 && _by + BALL_SZ > gy0) {
        _level_done = true;
        _flash_until_us = now + FLASH_MS * 1000;
        lv_obj_clear_flag(_flash, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(_flash);
    }
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, TiltMazeApp, APP_NAME, []()
{
    return std::shared_ptr<TiltMazeApp>(TiltMazeApp::requestInstance(), [](TiltMazeApp *) {});
})

} // namespace esp_brookesia::apps
