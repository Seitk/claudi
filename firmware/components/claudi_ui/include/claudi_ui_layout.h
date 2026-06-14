// claudi_ui_layout.h — pure (hardware-free) responsive geometry so the same app
// code lays out on any resolution/shape. Host-testable; no LVGL/IDF deps.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { CLAUDI_LAYOUT_ROUND, CLAUDI_LAYOUT_RECT } claudi_layout_shape_t;

typedef struct {
    int16_t cx, cy;        // screen center
    int16_t short_side;    // min(w,h)
    int16_t safe_radius;   // round: inscribed radius; rect: half short side
} claudi_layout_t;

// Compute the geometry anchors for a given live display.
claudi_layout_t claudi_layout_compute(int16_t w, int16_t h, claudi_layout_shape_t shape);

// LVGL zoom factor (256 == 1.0x) to render a `src_px` asset at `frac` of the
// short side. e.g. zoom for the pet image.
uint16_t claudi_layout_image_zoom(int16_t short_side, int16_t src_px, float frac);

#ifdef __cplusplus
}
#endif
