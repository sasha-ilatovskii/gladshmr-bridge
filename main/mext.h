#pragma once
// mext protocol constants — derived from libmonome src/proto/mext.h
// Header byte = (subsystem << 4) | command
// All values verified against libmonome source.

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Subsystem nibbles
// ---------------------------------------------------------------------------
#define SS_SYSTEM 0
#define SS_LED_GRID 1
#define SS_KEY_GRID 2
#define SS_TILT 8 // libmonome mext.h: SS_TILT = 8
#define SS_ENCODER 5
#define SS_LED_RING 9

// ---------------------------------------------------------------------------
// Message header bytes (SS << 4 | CMD)
//
// HOST → DEVICE (commands from libmonome/norns)
// ---------------------------------------------------------------------------
#define MEXT_SYSTEM_QUERY 0x00      // no payload; device responds with QUERY_RESPONSE
#define MEXT_SYSTEM_GET_ID 0x01     // no payload; device responds with SYSTEM_ID
#define MEXT_SYSTEM_GET_GRIDSZ 0x05 // no payload; device responds with SYSTEM_GRIDSZ

#define MEXT_LED_OFF 0x10          // payload: x(1) y(1)
#define MEXT_LED_ON 0x11           // payload: x(1) y(1)
#define MEXT_LED_ALL_OFF 0x12      // no payload
#define MEXT_LED_ALL_ON 0x13       // no payload
#define MEXT_LED_MAP 0x14          // payload: x_off(1) y_off(1) data[8]   — bitmask
#define MEXT_LED_ROW 0x15          // payload: x_off(1) y(1) data(1)
#define MEXT_LED_COLUMN 0x16       // payload: x(1) y_off(1) data(1)
#define MEXT_LED_INTENSITY 0x17    // payload: intensity(1)  — 0-15
#define MEXT_LED_LEVEL_SET 0x18    // payload: x(1) y(1) level(1)
#define MEXT_LED_LEVEL_ALL 0x19    // payload: level(1)
#define MEXT_LED_LEVEL_MAP 0x1A    // payload: x_off(1) y_off(1) levels[32] — 64×4-bit packed
#define MEXT_LED_LEVEL_ROW 0x1B    // payload: x_off(1) y(1) levels[4]      — 8×4-bit packed
#define MEXT_LED_LEVEL_COLUMN 0x1C // payload: x(1) y_off(1) levels[4]

// Tilt commands (SS_TILT=8):
// header = (SS_TILT << 4) | cmd
// CMD_TILT_ENABLE=1 → 0x81   HOST→DEVICE  payload: sensor(1)
// CMD_TILT_DISABLE=2 → 0x82  HOST→DEVICE  payload: sensor(1)
// CMD_TILT=1        → 0x81   DEVICE→HOST  payload: sensor(1) x(2LE) y(2LE) z(2LE)
// TILT_ENABLE and TILT (data) share opcode 0x81; direction determines meaning.
#define MEXT_TILT_ENABLE 0x81  // HOST→DEVICE payload: sensor(1)
#define MEXT_TILT_DISABLE 0x82 // HOST→DEVICE payload: sensor(1)
#define MEXT_TILT 0x81         // DEVICE→HOST payload: sensor(1) x(2LE) y(2LE) z(2LE)

#define MEXT_RING_SET 0x90       // payload: ring(1) led(1) level(1)
#define MEXT_RING_ALL 0x91       // payload: ring(1) level(1)
#define MEXT_RING_MAP 0x92       // payload: ring(1) levels[32] — 64×4-bit packed
#define MEXT_RING_RANGE 0x93     // payload: ring(1) start(1) end(1) level(1)
#define MEXT_RING_INTENSITY 0x94 // payload: ring(1) level(1)

// ---------------------------------------------------------------------------
// DEVICE → HOST (responses and input events)
// ---------------------------------------------------------------------------
#define MEXT_SYSTEM_QUERY_RESPONSE 0x00 // payload: subsystem(1) count(1)
#define MEXT_SYSTEM_ID 0x01             // payload: id[32] (null-padded ASCII)
#define MEXT_SYSTEM_GRIDSZ 0x03         // payload: cols(1) rows(1)

#define MEXT_KEY_UP 0x20   // payload: x(1) y(1)
#define MEXT_KEY_DOWN 0x21 // payload: x(1) y(1)

// MEXT_TILT / MEXT_TILT_ENABLE / MEXT_TILT_DISABLE defined above (0x81 / 0x82)

#define MEXT_ENCODER_DELTA 0x50       // payload: number(1) delta(1, signed int8)
#define MEXT_ENCODER_SWITCH_UP 0x51   // payload: number(1)
#define MEXT_ENCODER_SWITCH_DOWN 0x52 // payload: number(1)

// ---------------------------------------------------------------------------
// Grid / arc size constants — must precede MEXT_INCOMING_PAYLOAD_LEN
// ---------------------------------------------------------------------------
#define GRID_COLS 16
#define GRID_ROWS 8
#define ARC_RINGS 4
#define ARC_COLS 0 // report 0 cols → norns detects as arc
#define ARC_ROWS 0 // report 0 rows → norns detects as arc

// Device ID strings (null-padded to 32 bytes by the sender)
#define GRID_ID_STR "m1000001"
#define ARC_ID_STR "a4000001"

// ---------------------------------------------------------------------------
// Payload lengths for incoming (host → device) messages
// Used by the parser to know how many bytes follow the header byte.
// Row/column lengths depend on grid dimensions (computed from GRID_COLS/ROWS).
// ---------------------------------------------------------------------------
static const uint8_t MEXT_INCOMING_PAYLOAD_LEN[256] = {
    [MEXT_SYSTEM_QUERY] = 0,
    [MEXT_SYSTEM_GET_ID] = 0,
    [MEXT_SYSTEM_GET_GRIDSZ] = 0,

    [MEXT_LED_OFF] = 2,
    [MEXT_LED_ON] = 2,
    [MEXT_LED_ALL_OFF] = 0,
    [MEXT_LED_ALL_ON] = 0,
    [MEXT_LED_MAP] = 10,
    [MEXT_LED_ROW] = 3,    // x_off(1) y(1) data(1) — libmonome sends 8-pixel chunks
    [MEXT_LED_COLUMN] = 3, // x(1) y_off(1) data(1) — libmonome sends 8-pixel chunks
    [MEXT_LED_INTENSITY] = 1,
    [MEXT_LED_LEVEL_SET] = 3,
    [MEXT_LED_LEVEL_ALL] = 1,
    [MEXT_LED_LEVEL_MAP] = 34,
    [MEXT_LED_LEVEL_ROW] = 6,    // x_off(1) y(1) levels[4] — 8 pixels × 4-bit packed per chunk
    [MEXT_LED_LEVEL_COLUMN] = 6, // x(1) y_off(1) levels[4] — 8 pixels × 4-bit packed per chunk

    // SS_TILT=8 commands from norns (mext, not old protocol):
    // 0x80 = CMD_TILT_STATE_REQ (no payload)
    // 0x81 = CMD_TILT_ENABLE    payload: sensor(1)
    // 0x82 = CMD_TILT_DISABLE   payload: sensor(1)
    [0x80] = 0,
    [MEXT_TILT_ENABLE] = 1,
    [MEXT_TILT_DISABLE] = 1,

    [MEXT_RING_SET] = 3,
    [MEXT_RING_ALL] = 2,
    [MEXT_RING_MAP] = 33,
    [MEXT_RING_RANGE] = 4,
    [MEXT_RING_INTENSITY] = 2,
};
