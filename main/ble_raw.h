#pragma once
#include <stdint.h>
// Raw BLE peripheral — custom GATT service (NimBLE)
//
// Replaces the BLE MIDI service with a custom service that uses a simple
// binary packet format. No MIDI framing, no timestamp bytes — just raw
// event bytes written directly to the data characteristic.
//
// UUIDs (custom, "monome" encoded in the high bytes):
//   Service:     6D6F6E6F-6D65-0000-0000-000000000000
//   Data char:   6D6F6E6F-6D65-0000-0000-000000000001  (Write Without Response)
//   Config char: 6D6F6E6F-6D65-0000-0000-000000000002  (Write | Write Without Response)
//
// Data packet format (iPad → ESP32):
//   Byte 0:  event type
//   Byte 1+: payload
//
//   0x10  KEY_DOWN     [x(1), y(1)]                               3 bytes total
//   0x11  KEY_UP       [x(1), y(1)]                               3 bytes total
//   0x20  ENC_DELTA    [num(1), delta(1)]                         3 bytes  (delta is int8 cast to
//   uint8) 0x21  ENC_SW_DOWN  [num(1)]                                   2 bytes 0x22  ENC_SW_UP
//   [num(1)]                                   2 bytes 0x30  TILT         [sensor(1), x_lo(1),
//   x_hi(1),             8 bytes  (x/y/z are 16-bit LE signed)
//                       y_lo(1), y_hi(1), z_lo(1), z_hi(1)]
//
// Config characteristic format: identical to firmware-ble (monome_slots format):
//   [count, type p1 p2, type p1 p2, ...]
//
// Advertising name: "monome-raw"

void ble_raw_init(void);

// Toggle verbose logging of incoming BLE raw packets.
// Intended for runtime debugging via the ESP32 BOOT button.
void ble_raw_toggle_debug(void);

// Send a state notification to the connected iPad (ESP32 → iPad).
// data is the raw mext bytes: [header_byte, payload...].
// No-op if no client is connected or not subscribed to notifications.
void ble_raw_send_state(const uint8_t* data, uint16_t len);
