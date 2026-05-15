// Wire protocol shared with the Mac daemon. Mirror of crates/protocol/src/lib.rs.
//
// Frame layout (LE everywhere):
//   [u8 type][u24 length][payload...]

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLAUDI_DISPLAY_W  368
#define CLAUDI_DISPLAY_H  448
#define CLAUDI_PROTOCOL_VERSION 1
#define CLAUDI_MAX_PAYLOAD  (64 * 1024)

enum {
    CLAUDI_MSG_HELLO       = 0x01,
    CLAUDI_MSG_FRAME_BEGIN = 0x10,
    CLAUDI_MSG_RECT        = 0x11,
    CLAUDI_MSG_RECT_RLE    = 0x12,
    CLAUDI_MSG_FRAME_END   = 0x13,
    CLAUDI_MSG_BACKLIGHT   = 0x20,
    CLAUDI_MSG_BEEP        = 0x21,
    CLAUDI_MSG_SET_TIME    = 0x22,
    CLAUDI_MSG_REBOOT      = 0x23,  // payload: u8 mode (0=app reboot, 1=BOOTSEL)
    CLAUDI_MSG_PING        = 0x30,

    CLAUDI_MSG_HELLO_ACK   = 0x81,
    CLAUDI_MSG_TOUCH       = 0x90,
    CLAUDI_MSG_GESTURE     = 0x91,
    CLAUDI_MSG_PONG        = 0xB0,
    CLAUDI_MSG_LOG         = 0xC0,
};

enum {
    CLAUDI_TOUCH_DOWN = 0,
    CLAUDI_TOUCH_MOVE = 1,
    CLAUDI_TOUCH_UP   = 2,
};

// Streaming decoder for a single connection.
// Feed bytes via claudi_proto_feed(); the callback fires for each fully decoded
// message. Pointers inside the callback are valid for the call's duration only.
typedef struct claudi_proto_ctx claudi_proto_ctx;

typedef void (*claudi_proto_on_msg)(claudi_proto_ctx *ctx,
                                    uint8_t type,
                                    const uint8_t *payload,
                                    uint32_t len,
                                    void *user);

struct claudi_proto_ctx {
    uint8_t  header[4];
    uint8_t  header_pos;
    uint8_t  payload[CLAUDI_MAX_PAYLOAD];
    uint32_t payload_len;
    uint32_t payload_pos;
    uint8_t  msg_type;
    claudi_proto_on_msg cb;
    void    *user;
};

void claudi_proto_init(claudi_proto_ctx *ctx, claudi_proto_on_msg cb, void *user);
void claudi_proto_feed(claudi_proto_ctx *ctx, const uint8_t *data, size_t len);

// Encode helpers. Each writes a complete framed message (header + payload)
// into `out` and returns the total bytes written.
size_t claudi_proto_enc_hello_ack(uint8_t *out, uint16_t fw_version, uint16_t w, uint16_t h, uint32_t features);
size_t claudi_proto_enc_touch(uint8_t *out, uint8_t phase, uint16_t x, uint16_t y, uint32_t ts_ms);
size_t claudi_proto_enc_pong(uint8_t *out, uint32_t seq);
size_t claudi_proto_enc_log(uint8_t *out, const char *text);
