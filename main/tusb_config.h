#pragma once

// ---------------------------------------------------------------
// TinyUSB device configuration — BLE bridge (single board)
// Two CDC-ACM interfaces; norns detects both as "monome" devices.
// The mext handshake differentiates grid (16×8) from arc (4 enc).
// ---------------------------------------------------------------

#define CFG_TUSB_RHPORT0_MODE   OPT_MODE_DEVICE
#define CFG_TUSB_OS             OPT_OS_FREERTOS

#define CFG_TUSB_DEBUG          0

// Two CDC-ACM virtual serial ports
#define CFG_TUD_CDC             2
#define CFG_TUD_CDC_RX_BUFSIZE  2048
#define CFG_TUD_CDC_TX_BUFSIZE  1024

// Unused device classes
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_MSC             0
#define CFG_TUD_VENDOR          0
