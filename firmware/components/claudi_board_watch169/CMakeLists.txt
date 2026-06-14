# See claudi_board_amoled175/CMakeLists.txt: REQUIRES must be literal (IDF's early
# requirement-expansion does not see CONFIG_*); only SRCS is gated on the board.
# Phase 2 adds the real esp_lcd/st7789 + cst816 + adc PRIV_REQUIRES here.
if(CONFIG_CLAUDI_BOARD_WATCH169)
    set(srcs "claudi_board_watch169.c")
else()
    set(srcs "")
endif()

idf_component_register(
    SRCS ${srcs}
    REQUIRES claudi_board driver lvgl
)
