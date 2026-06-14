# Interface-only: the chosen impl component provides the function bodies.
if(CONFIG_CLAUDI_BOARD_WATCH169)
    set(claudi_board_impl claudi_board_watch169)
else()
    set(claudi_board_impl claudi_board_amoled175)
endif()

idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES driver lvgl ${claudi_board_impl}
)
