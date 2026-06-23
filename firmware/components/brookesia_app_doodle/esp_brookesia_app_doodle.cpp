/*
 * Doodle finger-paint toy — see header. Round 466x466 AMOLED.
 *
 * A full-screen white lv_canvas (PSRAM buffer) is the drawing surface. Dragging
 * a finger stamps filled brush discs and interpolates between samples so fast
 * strokes stay solid. A bottom row of bright color swatches (plus a white
 * eraser) sets the brush; a top "Clear" button refills the canvas white.
 * Swatches/Clear are created after the canvas so they sit on top and capture
 * their own taps — the canvas only receives drags on the open drawing area.
 */
#include "esp_brookesia_app_doodle.hpp"

#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:doodle"
#include "esp_lib_utils.h"

#include "esp_timer.h"
#include "esp_random.h"
#include "game_icons.h"
#include "esp_heap_caps.h"

#include <cmath>
#include <cstring>

#define APP_NAME "Doodle"

// --- Canvas geometry (screen is 466x466, center 233,233, visible radius ~233) ---
static const int W = 466, H = 466;
#define BRUSH_R  7      // brush disc radius in pixels
#define STEP_PX  3.0f   // interpolation step along a stroke

// --- Palette: 6 bright colors + an eraser (white). ---
#define COL_ERASER  0xFFFFFF
static const uint32_t kSwatchColors[7] = {
    0x111111,  // black
    0xFF3B30,  // red
    0xFF9500,  // orange
    0xFFD60A,  // yellow
    0x34C759,  // green
    0x0A84FF,  // blue
    COL_ERASER // eraser (white) — last swatch
};
#define ERASER_INDEX 6
#define SWATCH_COUNT 7

// --- Swatch layout: an arc hugging the bottom of the round glass so all seven
//     big swatches stay fully on-screen (a straight row would push the outer
//     ones past the visible circle on a 466px round display). Centers sit on a
//     circle of radius SWATCH_ARC_R around the screen center; SWATCH_ARC_STEP is
//     the angle between neighbours, fanned symmetrically about straight-down. ---
#define SWATCH_SIZE 44
#define SWATCH_ARC_R   197.0f  // center distance from screen center (edge ~219 < 225)
#define SWATCH_ARC_STEP 0.2640f // ~15.1 deg between adjacent swatches

#define COL_RING    0x111111   // selection ring on a swatch
#define COL_CLEAR_BG 0x222230   // Clear button background

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

namespace esp_brookesia::apps {

DoodleApp *DoodleApp::_instance = nullptr;

DoodleApp *DoodleApp::requestInstance()
{
    if (_instance == nullptr) {
        _instance = new DoodleApp();
    }
    return _instance;
}

DoodleApp::DoodleApp(): App(APP_NAME, &icon_doodle, true, false, false)
{
    _color = lv_color_hex(0x111111);  // start on black
}

DoodleApp::~DoodleApp()
{
    if (_canvas_buf) {
        heap_caps_free(_canvas_buf);
        _canvas_buf = nullptr;
    }
}

bool DoodleApp::close(void)
{
    // Returning to the launcher: release the large PSRAM canvas buffer so it does
    // not leak across open/close cycles. Delete the canvas first so nothing draws
    // from the buffer after it is freed; the core recycles the rest. run() will
    // reallocate a fresh buffer next time the app is opened.
    if (_canvas) {
        lv_obj_del(_canvas);
        _canvas = nullptr;
    }
    if (_canvas_buf) {
        heap_caps_free(_canvas_buf);
        _canvas_buf = nullptr;
    }
    return true;
}

bool DoodleApp::run(void)
{
    ESP_UTILS_LOGI("doodle run");

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // --- Full-screen canvas backed by a PSRAM pixel buffer. ---
    uint32_t stride = lv_draw_buf_width_to_stride(W, LV_COLOR_FORMAT_RGB565);
    _canvas_buf = heap_caps_aligned_alloc(64, (size_t)stride * H, MALLOC_CAP_SPIRAM);
    if (_canvas_buf == nullptr) {
        // Out of PSRAM: bail gracefully so the launcher stays usable.
        ESP_UTILS_LOGE("doodle: canvas buffer alloc failed");
        return true;
    }

    _canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(_canvas, _canvas_buf, W, H, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(_canvas, lv_color_hex(0xFFFFFF), LV_OPA_COVER);
    lv_obj_align(_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(_canvas, LV_OBJ_FLAG_SCROLLABLE);

    // Painting is purely event-driven: press starts a stroke, pressing extends
    // it, release/loss ends it so the next stroke does not connect.
    lv_obj_add_event_cb(_canvas, onCanvasPress, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(_canvas, onCanvasDraw, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(_canvas, onCanvasRelease, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(_canvas, onCanvasRelease, LV_EVENT_PRESS_LOST, this);

    // --- Color swatches, fanned along the bottom arc (on top of the canvas). ---
    for (int i = 0; i < SWATCH_COUNT; ++i) {
        // Angle for swatch i, centered about straight-down (pi/2) and symmetric.
        float ang = (float)M_PI / 2.0f +
                    ((float)i - (float)(SWATCH_COUNT - 1) / 2.0f) * SWATCH_ARC_STEP;
        int sx = (int)(SWATCH_ARC_R * cosf(ang));  // offset from screen center
        int sy = (int)(SWATCH_ARC_R * sinf(ang));
        lv_obj_t *sw = lv_obj_create(scr);
        _swatches[i] = sw;
        lv_obj_set_size(sw, SWATCH_SIZE, SWATCH_SIZE);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(sw, 0, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(kSwatchColors[i]), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        // Eraser gets a dark outline so kids can see the white swatch.
        if (i == ERASER_INDEX) {
            lv_obj_set_style_border_color(sw, lv_color_hex(0x999999), 0);
            lv_obj_set_style_border_width(sw, 2, 0);
        } else {
            lv_obj_set_style_border_width(sw, 0, 0);
        }
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(sw, LV_ALIGN_CENTER, sx, sy);
        lv_obj_add_event_cb(sw, onSwatch, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // An "X" label on the eraser swatch for extra clarity.
        if (i == ERASER_INDEX) {
            lv_obj_t *x = lv_label_create(sw);
            lv_label_set_text(x, "X");
            lv_obj_set_style_text_font(x, &lv_font_montserrat_22, 0);
            lv_obj_set_style_text_color(x, lv_color_hex(0x999999), 0);
            lv_obj_center(x);
            lv_obj_clear_flag(x, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    // --- Clear button, top center, visually distinct and on top. ---
    lv_obj_t *clear_btn = lv_button_create(scr);
    lv_obj_set_size(clear_btn, 110, 48);
    lv_obj_set_style_radius(clear_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(clear_btn, lv_color_hex(COL_CLEAR_BG), 0);
    lv_obj_set_style_bg_opa(clear_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(clear_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(clear_btn, 2, 0);
    lv_obj_align(clear_btn, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_add_event_cb(clear_btn, onClear, LV_EVENT_CLICKED, this);

    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_label_set_text(clear_lbl, "Clear");
    lv_obj_set_style_text_font(clear_lbl, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(clear_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(clear_lbl);

    // Start with black selected and its highlight ring shown.
    selectSwatch(0);
    return true;
}

bool DoodleApp::back(void)
{
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "notify core closed failed");
    return true;
}

// Highlight the chosen swatch with a thick ring and set the brush color.
void DoodleApp::selectSwatch(int index)
{
    if (index < 0 || index >= SWATCH_COUNT) {
        return;
    }
    _selected = index;
    _color = lv_color_hex(kSwatchColors[index]);

    for (int i = 0; i < SWATCH_COUNT; ++i) {
        if (_swatches[i] == nullptr) {
            continue;
        }
        if (i == index) {
            // Selected: thick contrasting ring (white on dark, dark on light).
            uint32_t ring = (i == ERASER_INDEX) ? COL_RING : 0xFFFFFF;
            lv_obj_set_style_border_color(_swatches[i], lv_color_hex(ring), 0);
            lv_obj_set_style_border_width(_swatches[i], 5, 0);
        } else if (i == ERASER_INDEX) {
            // Unselected eraser keeps its faint outline so it stays visible.
            lv_obj_set_style_border_color(_swatches[i], lv_color_hex(0x999999), 0);
            lv_obj_set_style_border_width(_swatches[i], 2, 0);
        } else {
            lv_obj_set_style_border_width(_swatches[i], 0, 0);
        }
    }
}

// Stamp one filled disc of radius BRUSH_R at (cx, cy), clamped to the canvas.
void DoodleApp::stampDisc(int cx, int cy)
{
    int r2 = BRUSH_R * BRUSH_R;
    for (int dy = -BRUSH_R; dy <= BRUSH_R; ++dy) {
        int y = cy + dy;
        if (y < 0 || y >= H) {
            continue;
        }
        for (int dx = -BRUSH_R; dx <= BRUSH_R; ++dx) {
            if (dx * dx + dy * dy > r2) {
                continue;  // outside the disc
            }
            int x = cx + dx;
            if (x < 0 || x >= W) {
                continue;
            }
            lv_canvas_set_px(_canvas, x, y, _color, LV_OPA_COVER);
        }
    }
}

// Paint a solid line from the previous point to (x, y) by stepping discs.
void DoodleApp::strokeTo(int x, int y)
{
    if (!_has_last) {
        stampDisc(x, y);
        _last_x = x;
        _last_y = y;
        _has_last = true;
        return;
    }

    float dx = (float)(x - _last_x);
    float dy = (float)(y - _last_y);
    float dist = sqrtf(dx * dx + dy * dy);
    int steps = (int)(dist / STEP_PX);
    if (steps < 1) {
        steps = 1;
    }
    for (int i = 1; i <= steps; ++i) {
        float t = (float)i / (float)steps;
        stampDisc(_last_x + (int)(dx * t), _last_y + (int)(dy * t));
    }
    _last_x = x;
    _last_y = y;
}

void DoodleApp::onCanvasPress(lv_event_t *e)
{
    DoodleApp *self = static_cast<DoodleApp *>(lv_event_get_user_data(e));
    if (self == nullptr || self->_canvas == nullptr) {
        return;
    }
    // Fresh stroke: forget any previous point, then stamp the first dot.
    self->_has_last = false;
    lv_indev_t *in = lv_indev_active();
    if (in == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    self->strokeTo(p.x, p.y);
}

void DoodleApp::onCanvasDraw(lv_event_t *e)
{
    DoodleApp *self = static_cast<DoodleApp *>(lv_event_get_user_data(e));
    if (self == nullptr || self->_canvas == nullptr) {
        return;
    }
    lv_indev_t *in = lv_indev_active();
    if (in == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(in, &p);
    // Canvas sits at (0,0) full-screen, so screen point == canvas point.
    self->strokeTo(p.x, p.y);
}

void DoodleApp::onCanvasRelease(lv_event_t *e)
{
    DoodleApp *self = static_cast<DoodleApp *>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    self->_has_last = false;  // next stroke starts clean
}

void DoodleApp::onSwatch(lv_event_t *e)
{
    if (_instance == nullptr) {
        return;
    }
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    _instance->selectSwatch(index);
}

void DoodleApp::onClear(lv_event_t *e)
{
    DoodleApp *self = static_cast<DoodleApp *>(lv_event_get_user_data(e));
    if (self == nullptr || self->_canvas == nullptr) {
        return;
    }
    lv_canvas_fill_bg(self->_canvas, lv_color_hex(0xFFFFFF), LV_OPA_COVER);
    self->_has_last = false;
}

// Register the app (plugin macro; bare class name inside the namespace).
ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, DoodleApp, APP_NAME, []()
{
    return std::shared_ptr<DoodleApp>(DoodleApp::requestInstance(), [](DoodleApp *) {});
})

} // namespace esp_brookesia::apps
