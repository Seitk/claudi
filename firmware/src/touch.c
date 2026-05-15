// FT3168 capacitive touch driver — Pico SDK port of Waveshare's reference.

#include "touch.h"
#include "protocol.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define TOUCH_I2C_PORT  i2c1
#define DEV_SDA_PIN     6
#define DEV_SCL_PIN     7
#define TOUCH_RST_PIN   5
#define TOUCH_INT_PIN   4
#define FT3168_I2C_ADDR 0x38

// Registers
#define REG_FINGER_NUM  0x02
#define REG_X1_H        0x03
#define REG_Y1_H        0x05
#define REG_POWER_MODE  0xA5
#define REG_DEVICE_ID   0xA0

static void i2c_write_reg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    i2c_write_blocking(TOUCH_I2C_PORT, FT3168_I2C_ADDR, buf, 2, false);
}

static uint8_t i2c_read_reg(uint8_t reg) {
    uint8_t out = 0;
    i2c_write_blocking(TOUCH_I2C_PORT, FT3168_I2C_ADDR, &reg, 1, true);
    i2c_read_blocking(TOUCH_I2C_PORT, FT3168_I2C_ADDR, &out, 1, false);
    return out;
}

static void i2c_read_n(uint8_t reg, uint8_t *buf, uint32_t n) {
    i2c_write_blocking(TOUCH_I2C_PORT, FT3168_I2C_ADDR, &reg, 1, true);
    i2c_read_blocking(TOUCH_I2C_PORT, FT3168_I2C_ADDR, buf, n, false);
}

static void ft3168_reset(void) {
    gpio_put(TOUCH_RST_PIN, 1);
    sleep_ms(20);
    gpio_put(TOUCH_RST_PIN, 0);
    sleep_ms(20);
    gpio_put(TOUCH_RST_PIN, 1);
    sleep_ms(50);
}

bool touch_init(void) {
    // I2C @ 400 kHz
    i2c_init(TOUCH_I2C_PORT, 400 * 1000);
    gpio_set_function(DEV_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(DEV_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(DEV_SDA_PIN);
    gpio_pull_up(DEV_SCL_PIN);

    gpio_init(TOUCH_RST_PIN);
    gpio_set_dir(TOUCH_RST_PIN, GPIO_OUT);
    gpio_init(TOUCH_INT_PIN);
    gpio_set_dir(TOUCH_INT_PIN, GPIO_IN);
    gpio_pull_up(TOUCH_INT_PIN);

    ft3168_reset();
    i2c_write_reg(REG_POWER_MODE, 0x01);
    sleep_ms(20);
    return true;
}

// Edge state — only emit DOWN/MOVE/UP transitions, not raw polled points.
static bool was_down = false;
static uint16_t last_x = 0, last_y = 0;

bool touch_poll(touch_event_t *out) {
    uint8_t fingers = i2c_read_reg(REG_FINGER_NUM);

    if (fingers > 0) {
        uint8_t b[4];
        i2c_read_n(REG_X1_H, b, 4);
        uint16_t x = ((uint16_t)(b[0] & 0x0F) << 8) | b[1];
        uint16_t y = ((uint16_t)(b[2] & 0x0F) << 8) | b[3];

        if (!was_down) {
            was_down = true;
            last_x = x; last_y = y;
            out->phase = CLAUDI_TOUCH_DOWN;
            out->x = x; out->y = y;
            return true;
        } else if (x != last_x || y != last_y) {
            last_x = x; last_y = y;
            out->phase = CLAUDI_TOUCH_MOVE;
            out->x = x; out->y = y;
            return true;
        }
    } else if (was_down) {
        was_down = false;
        out->phase = CLAUDI_TOUCH_UP;
        out->x = last_x; out->y = last_y;
        return true;
    }
    return false;
}
