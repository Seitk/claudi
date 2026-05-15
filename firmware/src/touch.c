// Stub touch driver — real FT3168 I2C driver lands in a follow-up step.

#include "touch.h"

bool touch_init(void) {
    // TODO: FT3168 I2C init
    return true;
}

bool touch_poll(touch_event_t *out) {
    (void)out;
    return false;
}
