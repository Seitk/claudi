// claudi_board impl for the ESPWatch 1.69 (Waveshare ESP32-S3-Touch-LCD-1.69 class).
// 240x280 ST7789V2 over 4-wire SPI + CST816 I2C touch, wired into LVGL via
// esp_lvgl_port. Mirrors the contract main.cpp + the claudi_board HAL expect.
//
// PINOUT: the values below are the Waveshare-docs pin map for this board (the
// higher-confidence of the two published maps). A second blog reports a DIFFERENT
// map (SCK=18, CS=16, DC=2, RST=3, BL=17) and warns of board revisions, so the
// SPI/touch pins are the FIRST thing to confirm on the physical unit. They are all
// collected here so on-device tuning is a one-place edit.
#include "claudi_board.h"

#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"   // esp_lcd_new_panel_st7789
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lvgl_port.h"

static const char *TAG = "board.watch169";

// ---- Pinout (Waveshare ESP32-S3-Touch-LCD-1.69 docs map; verify on hardware) ----
#define PIN_LCD_SCLK   GPIO_NUM_6
#define PIN_LCD_MOSI   GPIO_NUM_7
#define PIN_LCD_DC     GPIO_NUM_4
#define PIN_LCD_CS     GPIO_NUM_5
#define PIN_LCD_RST    GPIO_NUM_8
#define PIN_LCD_BL     GPIO_NUM_15
#define PIN_I2C_SDA    GPIO_NUM_11
#define PIN_I2C_SCL    GPIO_NUM_10
#define PIN_TP_RST     GPIO_NUM_13
#define PIN_TP_INT     GPIO_NUM_14

#define LCD_HOST        SPI2_HOST
#define LCD_H_RES       240
#define LCD_V_RES       280
#define LCD_Y_GAP       20                 // 240x280 visible-row offset within 240x320 GRAM
#define LCD_PIXEL_CLK   (40 * 1000 * 1000)
#define LCD_CMD_BITS    8
#define LCD_PARAM_BITS  8

#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_RES     LEDC_TIMER_8_BIT   // 0..255 duty

// Brookesia launcher swipes recurse deeply on the LVGL task; the AMOLED needed
// 24 KB (BSP default 8 KB overflowed). Match that here.
#define LVGL_TASK_STACK (24 * 1024)

// Bring-up gate: set 0 to skip CST816 touch init (isolates the display path so
// the screen can be confirmed even if the touch I2C pins are wrong). Re-enable
// once the touch pinout is verified on hardware.
#ifndef WATCH_ENABLE_TOUCH
#define WATCH_ENABLE_TOUCH 0   // TEMP: off for display bring-up; re-enable once touch pins verified
#endif

static i2c_master_bus_handle_t s_i2c_bus = NULL;

// Lazy-create the shared I2C master bus (touch + IMU + RTC live on it).
static i2c_master_bus_handle_t i2c_bus_get(void)
{
    if (s_i2c_bus == NULL) {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = PIN_I2C_SDA,
            .scl_io_num = PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = { .enable_internal_pullup = true },
        };
        esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
            s_i2c_bus = NULL;
        }
    }
    return s_i2c_bus;
}

lv_display_t *claudi_board_display_start(void)
{
    // --- SPI bus ---
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * (int)sizeof(uint16_t),
    };
    if (spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed");
        return NULL;
    }

    // --- panel IO (SPI) ---
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .pclk_hz = LCD_PIXEL_CLK,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io) != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed");
        return NULL;
    }

    // --- ST7789 panel ---
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,  // -> _BGR if red/blue look swapped
        .bits_per_pixel = 16,
    };
    if (esp_lcd_new_panel_st7789(io, &panel_cfg, &panel) != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed");
        return NULL;
    }
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);          // ST7789 IPS power up color-inverted
    esp_lcd_panel_set_gap(panel, 0, LCD_Y_GAP);        // 240x280 row offset
    esp_lcd_panel_swap_xy(panel, false);
    esp_lcd_panel_mirror(panel, false, false);
    esp_lcd_panel_disp_on_off(panel, true);
    ESP_LOGI(TAG, "CHK: ST7789 panel up");

    // --- LVGL port (owns the LVGL task, tick, and lock) ---
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = LVGL_TASK_STACK;
    if (lvgl_port_init(&lvgl_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        return NULL;
    }
    ESP_LOGI(TAG, "CHK: lvgl_port_init done");

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 40,     // pixels
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = true,            // RGB565 over SPI needs byte swap
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return NULL;
    }
    ESP_LOGI(TAG, "CHK: lvgl_port_add_disp done");

    // --- CST816 touch on the shared I2C bus ---
#if WATCH_ENABLE_TOUCH
    i2c_master_bus_handle_t bus = i2c_bus_get();
    ESP_LOGI(TAG, "CHK: i2c bus %s", bus ? "ok" : "FAILED");
    if (bus != NULL) {
        esp_lcd_panel_io_handle_t tp_io = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
        if (esp_lcd_new_panel_io_i2c(bus, &tp_io_cfg, &tp_io) == ESP_OK) {
            ESP_LOGI(TAG, "CHK: touch panel-io i2c ok, init cst816...");
            esp_lcd_touch_handle_t tp = NULL;
            esp_lcd_touch_config_t tp_cfg = {
                .x_max = LCD_H_RES,
                .y_max = LCD_V_RES,
                .rst_gpio_num = PIN_TP_RST,
                .int_gpio_num = PIN_TP_INT,
                .levels = { .reset = 0, .interrupt = 0 },
                .flags = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
            };
            // CST816 sleeps when idle and only ACKs after a touch; a NACK here at
            // boot is expected, not fatal — keep the handle if we got one.
            esp_err_t terr = esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp);
            if (terr != ESP_OK) {
                ESP_LOGW(TAG, "CST816 init: %s (idle NACK is normal; wakes on first touch)",
                         esp_err_to_name(terr));
            }
            if (tp != NULL) {
                lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = tp };
                if (lvgl_port_add_touch(&touch_cfg) == NULL) {
                    ESP_LOGW(TAG, "lvgl_port_add_touch failed; touch disabled");
                }
            }
        } else {
            ESP_LOGW(TAG, "touch panel-io create failed; touch disabled");
        }
    }
#else
    ESP_LOGW(TAG, "touch DISABLED for bring-up (WATCH_ENABLE_TOUCH=0)");
#endif

    ESP_LOGI(TAG, "CHK: touch setup done");
    claudi_board_backlight_on();
    ESP_LOGI(TAG, "CHK: display_start returning");
    return disp;
}

esp_err_t claudi_board_backlight_on(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode = BL_LEDC_MODE,
        .timer_num = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        return err;
    }
    ledc_channel_config_t ccfg = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 255,            // full brightness
        .hpoint = 0,
    };
    return ledc_channel_config(&ccfg);
}

bool claudi_board_lock(int timeout_ms)
{
    // lvgl_port_lock(0) waits forever; map negative/zero -> 0, positive -> ms.
    return lvgl_port_lock(timeout_ms <= 0 ? 0 : (uint32_t)timeout_ms);
}

void claudi_board_unlock(void)
{
    lvgl_port_unlock();
}

i2c_master_bus_handle_t claudi_board_i2c_handle(void)
{
    return i2c_bus_get();
}

// No I2C PMU on this board; battery would come from an ADC divider on GPIO1.
// Stubbed until the divider ratio is confirmed on hardware (the pet hides the
// battery bubble when this returns -1).
int  claudi_board_battery_percent(void) { return -1; }
bool claudi_board_charging(void)        { return false; }

const claudi_board_info_t *claudi_board_info(void)
{
    static const claudi_board_info_t info = {
        .name = "watch169", .width = LCD_H_RES, .height = LCD_V_RES,
        .shape = CLAUDI_SHAPE_RECT, .has_pmu = false, .has_rtc = true,
    };
    return &info;
}
