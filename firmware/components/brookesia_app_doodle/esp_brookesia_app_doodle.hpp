/*
 * Doodle — a finger-paint creative toy for kids, shipped as an ESP-Brookesia
 * phone app (self-registers into the launcher). Round 466x466 AMOLED.
 *
 * The whole screen is a white canvas the child paints on by dragging a finger.
 * A row of bright color swatches plus an eraser sits near the bottom; a small
 * "Clear" button at top wipes the canvas. No score, no failure — endless play.
 */
#pragma once

#include "lvgl.h"
#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class DoodleApp: public systems::phone::App {
public:
    static DoodleApp *requestInstance();
    ~DoodleApp();

protected:
    DoodleApp();

    // Build the UI on the default screen (lv_scr_act()).
    bool run(void) override;
    // Back gesture: leave the toy, return to launcher.
    bool back(void) override;
    // On close (home/back): free the PSRAM canvas buffer. The core recycles the
    // LVGL widgets/timer, but this raw allocation is ours to release.
    bool close(void) override;

private:
    static DoodleApp *_instance;

    // Canvas drag-to-paint: continuous press strokes a line of brush discs.
    static void onCanvasPress(lv_event_t *e);   // start a fresh stroke
    static void onCanvasDraw(lv_event_t *e);     // stamp/interpolate while held
    static void onCanvasRelease(lv_event_t *e);  // end the stroke
    void strokeTo(int x, int y);                 // line from last point to (x,y)
    void stampDisc(int cx, int cy);              // one filled brush disc

    // Palette + clear handlers.
    static void onSwatch(lv_event_t *e);         // pick a color (user_data = index)
    static void onClear(lv_event_t *e);          // wipe canvas to white
    void selectSwatch(int index);                // set color + highlight ring

    // Widgets (created in run(); recycled by the core on app close).
    lv_obj_t *_canvas = nullptr;
    void *_canvas_buf = nullptr;                 // PSRAM-backed pixel buffer
    lv_obj_t *_swatches[7] = {nullptr};          // 6 colors + eraser

    // Paint state.
    lv_color_t _color;                           // current brush color
    int _selected = 0;                           // highlighted swatch index
    bool _has_last = false;                       // is _last_x/_last_y valid?
    int _last_x = 0, _last_y = 0;                 // previous touch point
};

} // namespace esp_brookesia::apps
