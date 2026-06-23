# Tilt Maze ESP-Brookesia game app. WHOLE_ARCHIVE so the plugin self-registration
# static-init runs (same as the claudi app); needs the Brookesia core plus the
# on-board IMU component (claudi_imu) for tilt input.
set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)

idf_component_register(
    SRCS ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES brookesia_core game_icons claudi_imu
    WHOLE_ARCHIVE
)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
