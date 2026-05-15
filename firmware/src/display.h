// SH8601 1.8" AMOLED 368x448 driver.

#pragma once

#include <stdbool.h>
#include <stdint.h>

bool display_init(void);
void display_set_brightness(uint8_t level);
void display_blit_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *rgb565_le);
