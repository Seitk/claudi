cmake_minimum_required(VERSION 3.13)

# Board profile that ships with the SDK — RP2350 + 16MB flash is the closest
# match for the Waveshare RP2350-Touch-AMOLED-1.8.
set(PICO_BOARD waveshare_rp2350_plus_16mb)
set(PICO_PLATFORM rp2350)

# Pull in SDK (must be before project())
include(pico_sdk_import.cmake)

project(claudi_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(claudi_firmware
    src/main.c
    src/usb_descriptors.c
    src/protocol.c
    src/display.c
    src/touch.c
    src/qspi_pio.c
)

target_include_directories(claudi_firmware PRIVATE src)

target_link_libraries(claudi_firmware
    pico_stdlib
    pico_unique_id
    pico_bootrom
    hardware_dma
    hardware_pio
    hardware_spi
    hardware_i2c
    hardware_pwm
    hardware_watchdog
    tinyusb_device
    tinyusb_board
)

# UART debug off — we use USB only.
pico_enable_stdio_usb(claudi_firmware 0)
pico_enable_stdio_uart(claudi_firmware 0)

# Generate UF2/ELF/BIN
pico_add_extra_outputs(claudi_firmware)
