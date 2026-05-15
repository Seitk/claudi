// TinyUSB configuration for the claudi firmware.

#pragma once

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU              OPT_MCU_RP2040
#endif

#define CFG_TUSB_OS               OPT_OS_PICO
#define CFG_TUSB_DEBUG            0

#define BOARD_TUD_RHPORT          0
#define BOARD_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED

#define CFG_TUD_ENABLED           1
#define CFG_TUD_MAX_SPEED         OPT_MODE_FULL_SPEED

#define CFG_TUD_ENDPOINT0_SIZE    64

#define CFG_TUD_CDC               1
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0

#define CFG_TUD_CDC_RX_BUFSIZE    4096
#define CFG_TUD_CDC_TX_BUFSIZE    4096
#define CFG_TUD_CDC_EP_BUFSIZE    64
