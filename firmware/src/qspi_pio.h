// PIO QSPI driver for the SH8601 AMOLED on the Waveshare RP2350-Touch-AMOLED-1.8.
// Ported from Waveshare's RP2350-Touch-AMOLED-1.8 Arduino reference.

#pragma once

#include <stdint.h>
#include "hardware/pio.h"
#include "hardware/gpio.h"

#define PIN_CS      9
#define PIN_SCLK    10
#define PIN_DIO0    11
#define PIN_DIO1    12
#define PIN_DIO2    13
#define PIN_DIO3    14
#define PIN_PWR_EN  17
#define PIN_RST     15

#define WAIT_TIME() do { for (int _i=0; _i<20; _i++) __asm__ volatile("nop"); } while (0)

typedef struct pio_qspi {
    PIO     pio;
    uint8_t sm;         // currently active state machine
    uint8_t sm_4wire;   // 4-line data state machine
    uint8_t sm_1wire;   // 1-line cmd state machine
    uint8_t pin_cs;
    uint8_t pin_sclk;
    uint8_t pin_dio0;
    uint8_t pin_dio1;
    uint8_t pin_dio2;
    uint8_t pin_dio3;
    uint8_t pin_pwr_en;
    uint8_t pin_rst;
} pio_qspi_t;

extern pio_qspi_t qspi;
extern int dma_tx;

void qspi_gpio_init(void);
void qspi_pio_init(void);
void qspi_select(void);
void qspi_deselect(void);
void qspi_1wire_mode(void);
void qspi_4wire_mode(void);
void qspi_cmd_write(uint32_t v);
void qspi_data_write(uint32_t v);
void qspi_register_write(uint32_t addr);
void qspi_pixel_write(uint32_t addr);
void qspi_dma_pixels(const uint8_t *bytes, uint32_t len);
