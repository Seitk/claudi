# Locate the Pico SDK. Honors PICO_SDK_PATH env var, falls back to ~/pico-sdk.
if (DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    message("Using PICO_SDK_PATH from environment: ${PICO_SDK_PATH}")
endif ()

if (NOT PICO_SDK_PATH)
    if (EXISTS "$ENV{HOME}/pico-sdk/pico_sdk_init.cmake")
        set(PICO_SDK_PATH "$ENV{HOME}/pico-sdk")
        message("Using PICO_SDK_PATH: ${PICO_SDK_PATH}")
    endif()
endif ()

if (NOT PICO_SDK_PATH)
    message(FATAL_ERROR
        "Pico SDK not found. Set PICO_SDK_PATH or clone to ~/pico-sdk.")
endif ()

set(PICO_SDK_INIT_CMAKE_FILE ${PICO_SDK_PATH}/pico_sdk_init.cmake)
if (NOT EXISTS ${PICO_SDK_INIT_CMAKE_FILE})
    message(FATAL_ERROR "Directory '${PICO_SDK_PATH}' does not appear to contain the Pico SDK")
endif ()

include(${PICO_SDK_INIT_CMAKE_FILE})
