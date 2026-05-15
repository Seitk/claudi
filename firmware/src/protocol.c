#include "protocol.h"
#include <string.h>

void claudi_proto_init(claudi_proto_ctx *ctx, claudi_proto_on_msg cb, void *user) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->cb = cb;
    ctx->user = user;
}

void claudi_proto_feed(claudi_proto_ctx *ctx, const uint8_t *data, size_t len) {
    while (len > 0) {
        if (ctx->header_pos < 4) {
            uint32_t need = 4 - ctx->header_pos;
            uint32_t take = (len < need) ? (uint32_t)len : need;
            memcpy(&ctx->header[ctx->header_pos], data, take);
            ctx->header_pos += take;
            data += take;
            len -= take;
            if (ctx->header_pos == 4) {
                ctx->msg_type    = ctx->header[0];
                ctx->payload_len = (uint32_t)ctx->header[1]
                                 | ((uint32_t)ctx->header[2] << 8)
                                 | ((uint32_t)ctx->header[3] << 16);
                ctx->payload_pos = 0;
                if (ctx->payload_len > CLAUDI_MAX_PAYLOAD) {
                    // Bad framing — drop and reset to resync.
                    ctx->header_pos = 0;
                    continue;
                }
                if (ctx->payload_len == 0 && ctx->cb) {
                    ctx->cb(ctx, ctx->msg_type, ctx->payload, 0, ctx->user);
                    ctx->header_pos = 0;
                }
            }
        } else {
            uint32_t need = ctx->payload_len - ctx->payload_pos;
            uint32_t take = (len < need) ? (uint32_t)len : need;
            memcpy(&ctx->payload[ctx->payload_pos], data, take);
            ctx->payload_pos += take;
            data += take;
            len -= take;
            if (ctx->payload_pos == ctx->payload_len) {
                if (ctx->cb) {
                    ctx->cb(ctx, ctx->msg_type, ctx->payload, ctx->payload_len, ctx->user);
                }
                ctx->header_pos = 0;
            }
        }
    }
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void put_u24_len(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
}

size_t claudi_proto_enc_hello_ack(uint8_t *out, uint16_t fw_version, uint16_t w, uint16_t h, uint32_t features) {
    out[0] = CLAUDI_MSG_HELLO_ACK;
    put_u24_len(&out[1], 10);
    put_u16(&out[4], fw_version);
    put_u16(&out[6], w);
    put_u16(&out[8], h);
    put_u32(&out[10], features);
    return 14;
}

size_t claudi_proto_enc_touch(uint8_t *out, uint8_t phase, uint16_t x, uint16_t y, uint32_t ts_ms) {
    out[0] = CLAUDI_MSG_TOUCH;
    put_u24_len(&out[1], 9);
    out[4] = phase;
    put_u16(&out[5], x);
    put_u16(&out[7], y);
    put_u32(&out[9], ts_ms);
    return 13;
}

size_t claudi_proto_enc_pong(uint8_t *out, uint32_t seq) {
    out[0] = CLAUDI_MSG_PONG;
    put_u24_len(&out[1], 4);
    put_u32(&out[4], seq);
    return 8;
}

size_t claudi_proto_enc_log(uint8_t *out, const char *text) {
    size_t n = 0;
    while (text[n] && n < 200) n++;
    out[0] = CLAUDI_MSG_LOG;
    put_u24_len(&out[1], (uint32_t)n);
    memcpy(&out[4], text, n);
    return 4 + n;
}
