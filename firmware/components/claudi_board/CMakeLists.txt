# Header-only HAL interface. The impl that defines these symbols is selected in
# main/CMakeLists.txt via CONFIG_CLAUDI_BOARD_* and required there — keeping the
# selection out of here avoids a circular dependency between this component and
# its impls (impl REQUIRES claudi_board for the header).
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES driver lvgl
)
