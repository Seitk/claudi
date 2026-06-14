# Build-time board selection: main requires the HAL header plus exactly one impl.
if(CONFIG_CLAUDI_BOARD_WATCH169)
    set(claudi_board_impl claudi_board_watch169)
else()
    set(claudi_board_impl claudi_board_amoled175)
endif()

idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core claudi_net claudi_core claudi_imu claudi_board ${claudi_board_impl}
)

target_compile_options(${COMPONENT_LIB} PUBLIC -Wno-missing-field-initializers)

# Silence LVGL's attribute warnings (matches the Waveshare example).
function(get_component_library component_name output_var)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    set(TARGET_COMPONENT "")
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "${component_name}" OR COMPONENT MATCHES "espressif__${component_name}")
            set(TARGET_COMPONENT ${COMPONENT})
            break()
        endif()
    endforeach()
    if(TARGET_COMPONENT STREQUAL "")
        message(FATAL_ERROR "Component '${component_name}' not found.")
    else()
        idf_component_get_property(COMPONENT_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
        set(${output_var} ${COMPONENT_LIB} PARENT_SCOPE)
    endif()
endfunction()

get_component_library("lvgl" LVGL_LIB)
target_compile_options(${LVGL_LIB} PUBLIC "-Wno-attributes")
