# Voice Plane ESP-Brookesia game app. WHOLE_ARCHIVE so the plugin self-registration
# static-init runs. Needs the Brookesia core plus the audio stack: the codec-dev API
# (esp_codec_dev_read/open) and the Waveshare BSP (bsp_audio_codec_microphone_init).
set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)

idf_component_register(
    SRCS ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES brookesia_core game_icons
    WHOLE_ARCHIVE
)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
