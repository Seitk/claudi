#include <assert.h>
#include <stdio.h>
#include "claudi_ui_layout.h"

int main(void) {
    // 466x466 round: center at (233,233), inscribed radius 233.
    claudi_layout_t r = claudi_layout_compute(466, 466, CLAUDI_LAYOUT_ROUND);
    assert(r.cx == 233 && r.cy == 233);
    assert(r.short_side == 466);
    assert(r.safe_radius == 233);

    // 240x280 rect: center at (120,140), safe area half the short side.
    claudi_layout_t w = claudi_layout_compute(240, 280, CLAUDI_LAYOUT_RECT);
    assert(w.cx == 120 && w.cy == 140);
    assert(w.short_side == 240);
    assert(w.safe_radius == 120);

    // Image zoom: smaller screen -> smaller pet; anchored to the old 466 value.
    uint16_t z466 = claudi_layout_image_zoom(466, 172, 0.66f);
    uint16_t z240 = claudi_layout_image_zoom(240, 172, 0.66f);
    assert(z240 < z466);
    assert(z466 == (uint16_t)((466 * 0.66f / 172.0f) * 256.0f));

    printf("all claudi_ui_layout tests passed\n");
    return 0;
}
