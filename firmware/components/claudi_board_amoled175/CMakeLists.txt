# IDF's early requirement-expansion pass does NOT see CONFIG_* values, so
# REQUIRES/PRIV_REQUIRES must be literal (unconditional). Only SRCS may be gated
# on CONFIG_ — it is evaluated later, at normal configure, where CONFIG_ is set.
# When this board is not selected, SRCS is empty so nothing here compiles, and
# main does not require this component, so the (literal) deps below cost nothing.
if(CONFIG_CLAUDI_BOARD_AMOLED175)
    set(srcs "claudi_board_amoled175.c")
else()
    set(srcs "")
endif()

idf_component_register(
    SRCS ${srcs}
    REQUIRES claudi_board driver lvgl
    PRIV_REQUIRES esp32_s3_touch_amoled_1_75 claudi_power
)
