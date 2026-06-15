# Compile the watch impl only when this board is selected. REQUIRES must be literal
# (IDF's early requirement-expansion does not see CONFIG_*); only SRCS is gated.
# esp_lcd (built-in ST7789 + SPI/I2C panel-io) + the LEDC/SPI/I2C drivers are IDF
# components; esp_lvgl_port + esp_lcd_touch_cst816s come from idf_component.yml.
if(CONFIG_CLAUDI_BOARD_WATCH169)
    set(srcs "claudi_board_watch169.c")
else()
    set(srcs "")
endif()

idf_component_register(
    SRCS ${srcs}
    REQUIRES claudi_board driver lvgl esp_lcd
            esp_driver_spi esp_driver_i2c esp_driver_ledc
            esp_lvgl_port esp_lcd_touch_cst816s
)
