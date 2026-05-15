// PIO QSPI driver — ported to plain C from Waveshare's RP2350 reference.

#include "qspi_pio.h"
#include "qspi.pio.h"
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

pio_qspi_t qspi = {
    .pio       = pio0,
    .sm        = 0,
    .sm_4wire  = 0,
    .sm_1wire  = 1,
    .pin_cs    = PIN_CS,
    .pin_sclk  = PIN_SCLK,
    .pin_dio0  = PIN_DIO0,
    .pin_dio1  = PIN_DIO1,
    .pin_dio2  = PIN_DIO2,
    .pin_dio3  = PIN_DIO3,
    .pin_pwr_en= PIN_PWR_EN,
    .pin_rst   = PIN_RST,
};
int dma_tx = -1;
static dma_channel_config dma_cfg;

void qspi_gpio_init(void) {
    gpio_init(qspi.pin_cs);
    gpio_pull_down(qspi.pin_cs);
    gpio_set_dir(qspi.pin_cs, GPIO_OUT);
    gpio_put(qspi.pin_cs, 1);

    gpio_init(qspi.pin_pwr_en);
    gpio_set_dir(qspi.pin_pwr_en, GPIO_OUT);
    gpio_put(qspi.pin_pwr_en, 1);

    gpio_init(qspi.pin_rst);
    gpio_set_dir(qspi.pin_rst, GPIO_OUT);
}

void qspi_pio_init(void) {
    uint offset4 = pio_add_program(qspi.pio, &qspi_4wire_data_program);
    qspi_4wire_data_program_init(qspi.pio, qspi.sm_4wire, offset4, PIN_SCLK, PIN_DIO0, 4);

    uint offset1 = pio_add_program(qspi.pio, &qspi_1write_cmd_program);
    qspi_1write_cmd_program_init(qspi.pio, qspi.sm_1wire, offset1, PIN_SCLK, PIN_DIO0, 1);
    pio_sm_clear_fifos(qspi.pio, qspi.sm_1wire);

    pio_sm_set_enabled(qspi.pio, qspi.sm_4wire, false);
    pio_sm_set_enabled(qspi.pio, qspi.sm_1wire, false);

    // Reserve a DMA channel for streaming pixels into the PIO TX FIFO.
    dma_tx = dma_claim_unused_channel(true);
    dma_cfg = dma_channel_get_default_config(dma_tx);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&dma_cfg, true);
    channel_config_set_write_increment(&dma_cfg, false);
}

void qspi_select(void)   { gpio_put(qspi.pin_cs, 0); }
void qspi_deselect(void) { gpio_put(qspi.pin_cs, 1); }

void qspi_1wire_mode(void) {
    pio_sm_set_enabled(qspi.pio, qspi.sm_4wire, false);
    pio_sm_set_enabled(qspi.pio, qspi.sm_1wire, true);
    qspi.sm = qspi.sm_1wire;
}

void qspi_4wire_mode(void) {
    pio_sm_set_enabled(qspi.pio, qspi.sm_1wire, false);
    pio_sm_set_enabled(qspi.pio, qspi.sm_4wire, true);
    qspi.sm = qspi.sm_4wire;
}

static inline void qspi_pio_write(uint32_t v) {
    pio_sm_put_blocking(qspi.pio, qspi.sm, v << 24);
}
void qspi_cmd_write(uint32_t v)  { qspi_pio_write(v); }
void qspi_data_write(uint32_t v) { qspi_pio_write(v); }

void qspi_register_write(uint32_t addr) {
    qspi_cmd_write(0x02);
    qspi_data_write(0x00);
    qspi_data_write(addr);
    qspi_data_write(0x00);
}

void qspi_pixel_write(uint32_t addr) {
    qspi_cmd_write(0x32);
    qspi_data_write(0x00);
    qspi_data_write(addr);
    qspi_data_write(0x00);
    WAIT_TIME();
}

void qspi_dma_pixels(const uint8_t *bytes, uint32_t len) {
    channel_config_set_dreq(&dma_cfg, pio_get_dreq(qspi.pio, qspi.sm, true));
    dma_channel_configure(
        dma_tx, &dma_cfg,
        &qspi.pio->txf[qspi.sm],
        bytes,
        len,
        true);
    while (dma_channel_is_busy(dma_tx)) tight_loop_contents();
}
