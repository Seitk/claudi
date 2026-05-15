// Stub display driver — real SH8601 QSPI driver lands in a follow-up step.
//
// This stub lets the firmware build and run USB CDC end-to-end while we
// validate the link layer on real hardware. display_blit_rect is a no-op;
// once we have the SPI/QSPI plumbing it gets replaced.

#include "display.h"
#include "pico/stdlib.h"

bool display_init(void) {
    // TODO: SH8601 QSPI init — see firmware/docs/sh8601-init.md
    return true;
}

void display_set_brightness(uint8_t level) {
    (void)level;
}

void display_blit_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *pixels) {
    (void)x; (void)y; (void)w; (void)h; (void)pixels;
}
