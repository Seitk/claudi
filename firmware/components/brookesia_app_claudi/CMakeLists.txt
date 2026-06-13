# claudi ESP-Brookesia app. WHOLE_ARCHIVE so the plugin self-registration runs
# (same as the squareline demo); REQUIRES our local logic + ingest components.
set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)
# Generated LVGL pet images (assets/*.c, produced by tools/gen_pet_assets.py).
file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/assets/*.c)

idf_component_register(
    SRCS ${PROJ_SRCS_CPP} ${PROJ_SRCS_C}
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES claudi_core claudi_net button
    WHOLE_ARCHIVE
)

set_source_files_properties(
    ${PROJ_SRCS_CPP} ${PROJ_SRCS_C}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
