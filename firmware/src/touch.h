// FT3168 touch controller driver.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t phase; // CLAUDI_TOUCH_* enum value
    uint16_t x;
    uint16_t y;
} touch_event_t;

bool touch_init(void);
// Returns true if a new event was read into *out.
bool touch_poll(touch_event_t *out);
