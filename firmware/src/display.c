// SH8601 1.8" AMOLED 368x448 driver — Pico SDK port of Waveshare's reference.

#include "display.h"
#include "qspi_pio.h"
#include "pico/stdlib.h"

static void amoled_reset(void) {
    gpio_put(qspi.pin_rst, 1);
    sleep_ms(50);
    gpio_put(qspi.pin_rst, 0);
    sleep_ms(50);
    gpio_put(qspi.pin_rst, 1);
    sleep_ms(300);
}

static void amoled_init_regs(void) {
    qspi_1wire_mode();

    qspi_select();
    qspi_register_write(0x11);   // SLPOUT
    sleep_ms(120);
    qspi_deselect();

    qspi_select();
    qspi_register_write(0x44);
    qspi_data_write(0x01);
    qspi_data_write(0xC5);
    qspi_deselect();

    qspi_select();
    qspi_register_write(0x35);   // TE on
    qspi_data_write(0x00);
    qspi_deselect();

    qspi_select();
    qspi_register_write(0x3A);   // 16bpp via QSPI
    qspi_data_write(0x55);
    qspi_deselect();

    qspi_select();
    qspi_register_write(0xC4);   // SPI mode control
    qspi_data_write(0x80);
    qspi_deselect();

    qspi_select();
    qspi_register_write(0x53);
    qspi_data_write(0x20);
    qspi_deselect();

    qspi_select();
    qspi_register_write(0x51);   // Brightness
    qspi_data_write(0xFF);
    qspi_deselect();

    qspi_select();
    qspi_register_write(0x29);   // Display on
    qspi_deselect();

    sleep_ms(10);
}

bool display_init(void) {
    qspi_gpio_init();
    qspi_pio_init();
    amoled_reset();
    amoled_init_regs();
    return true;
}

void display_set_brightness(uint8_t level) {
    qspi_1wire_mode();
    qspi_select();
    qspi_register_write(0x51);
    qspi_data_write(level);
    qspi_deselect();
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    qspi_select();
    qspi_register_write(0x2A);
    qspi_data_write(x0 >> 8);
    qspi_data_write(x0 & 0xFF);
    qspi_data_write((x1 - 1) >> 8);
    qspi_data_write((x1 - 1) & 0xFF);
    qspi_deselect();

    qspi_select();
    qspi_register_write(0x2B);
    qspi_data_write(y0 >> 8);
    qspi_data_write(y0 & 0xFF);
    qspi_data_write((y1 - 1) >> 8);
    qspi_data_write((y1 - 1) & 0xFF);
    qspi_deselect();

    qspi_select();
    qspi_register_write(0x2C);
    qspi_deselect();
    WAIT_TIME();
}

void display_blit_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *pixels) {
    uint16_t x1 = x + w;
    uint16_t y1 = y + h;
    qspi_1wire_mode();
    set_window(x, y, x1, y1);
    qspi_select();
    qspi_pixel_write(0x2C);

    qspi_4wire_mode();
    // RP2350 RGB565: device wants big-endian per pixel. The host sends
    // little-endian. We push as raw bytes — TODO swap if colors look wrong.
    qspi_dma_pixels(pixels, (uint32_t)w * (uint32_t)h * 2);
    qspi_deselect();
}
