// claudi firmware entry point.
//
// Stage 1 (this file): USB CDC handshake + protocol echo. Validates the link
// layer end-to-end against the Mac daemon. Display + touch are stubs.
// Stage 2: replace display.c stub with the SH8601 QSPI driver.
// Stage 3: replace touch.c stub with the FT3168 I2C driver.

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "protocol.h"
#include "display.h"
#include "touch.h"

#define FW_VERSION 1

#define HEARTBEAT_LED_PIN 25  // overridden by board profile if set

static claudi_proto_ctx g_proto;
static uint8_t          g_outbuf[256];

static void cdc_write_all(const uint8_t *data, uint32_t len) {
    uint32_t off = 0;
    while (off < len) {
        if (!tud_cdc_connected()) return;
        uint32_t avail = tud_cdc_write_available();
        if (avail == 0) {
            tud_cdc_write_flush();
            tud_task();
            continue;
        }
        uint32_t take = (len - off < avail) ? (len - off) : avail;
        uint32_t wrote = tud_cdc_write(data + off, take);
        off += wrote;
        if (wrote == 0) {
            tud_task();
        }
    }
    tud_cdc_write_flush();
}

static void on_msg(claudi_proto_ctx *ctx, uint8_t type, const uint8_t *p, uint32_t len, void *user) {
    (void)ctx; (void)user;
    switch (type) {
        case CLAUDI_MSG_HELLO: {
            size_t n = claudi_proto_enc_hello_ack(g_outbuf, FW_VERSION,
                                                  CLAUDI_DISPLAY_W, CLAUDI_DISPLAY_H,
                                                  0x7);
            cdc_write_all(g_outbuf, n);
            break;
        }
        case CLAUDI_MSG_PING: {
            if (len >= 4) {
                uint32_t seq = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                             | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                size_t n = claudi_proto_enc_pong(g_outbuf, seq);
                cdc_write_all(g_outbuf, n);
            }
            break;
        }
        case CLAUDI_MSG_FRAME_BEGIN:
        case CLAUDI_MSG_FRAME_END:
            // Frame boundaries — no-op until display driver is live.
            break;
        case CLAUDI_MSG_RECT: {
            if (len >= 8) {
                uint16_t x = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
                uint16_t y = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
                uint16_t w = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
                uint16_t h = (uint16_t)p[6] | ((uint16_t)p[7] << 8);
                display_blit_rect(x, y, w, h, p + 8);
            }
            break;
        }
        case CLAUDI_MSG_BACKLIGHT:
            if (len >= 1) display_set_brightness(p[0]);
            break;
        case CLAUDI_MSG_SET_TIME:
            // No RTC integration in stage 1.
            break;
        case CLAUDI_MSG_REBOOT:
            if (len >= 1) {
                if (p[0] == 1) {
                    // Reboot into the ROM USB bootloader (BOOTSEL).
                    // Acknowledge in a log message; reboot happens immediately after.
                    size_t n = claudi_proto_enc_log(g_outbuf, "rebooting to BOOTSEL");
                    cdc_write_all(g_outbuf, n);
                    // Brief settle so the log flushes before reset.
                    sleep_ms(50);
                    reset_usb_boot(0, 0);
                } else {
                    sleep_ms(50);
                    // soft reset
                    watchdog_reboot(0, 0, 0);
                }
            }
            break;
        default:
            // Ignore unknown — keeps forward compat.
            break;
    }
}

// Arduino convention: when the host opens the CDC port at 1200 baud, reboot to
// BOOTSEL. This is what `arduino-cli upload` does for many boards; it's also
// what `picotool reboot -u -f` falls back to when a custom interface isn't found.
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *p_line_coding) {
    (void)itf;
    if (p_line_coding && p_line_coding->bit_rate == 1200) {
        sleep_ms(50);
        reset_usb_boot(0, 0);
    }
}

static void pump_cdc_rx(void) {
    if (!tud_cdc_available()) return;
    uint8_t buf[256];
    uint32_t n = tud_cdc_read(buf, sizeof(buf));
    if (n > 0) claudi_proto_feed(&g_proto, buf, n);
}

int main(void) {
    stdio_init_all();

    gpio_init(HEARTBEAT_LED_PIN);
    gpio_set_dir(HEARTBEAT_LED_PIN, GPIO_OUT);

    display_init();
    touch_init();

    tud_init(BOARD_TUD_RHPORT);
    claudi_proto_init(&g_proto, on_msg, NULL);

    absolute_time_t next_blink = make_timeout_time_ms(0);
    bool led_on = false;
    absolute_time_t next_touch_poll = make_timeout_time_ms(0);

    while (true) {
        tud_task();
        pump_cdc_rx();

        if (absolute_time_diff_us(get_absolute_time(), next_blink) <= 0) {
            led_on = !led_on;
            gpio_put(HEARTBEAT_LED_PIN, led_on);
            next_blink = make_timeout_time_ms(tud_cdc_connected() ? 1000 : 200);
        }

        if (absolute_time_diff_us(get_absolute_time(), next_touch_poll) <= 0) {
            touch_event_t ev;
            if (touch_poll(&ev)) {
                size_t n = claudi_proto_enc_touch(g_outbuf, ev.phase, ev.x, ev.y,
                                                  (uint32_t)to_ms_since_boot(get_absolute_time()));
                cdc_write_all(g_outbuf, n);
            }
            next_touch_poll = make_timeout_time_ms(10);
        }
    }
}
