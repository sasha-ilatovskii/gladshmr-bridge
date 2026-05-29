#pragma once
// Internal event bus — replaces the inter-board UART bridge.
//
// Keeps the same uart_msg_t struct and UART_MSG_* constants so that
// mext_grid / mext_arc need no logic changes; only their include and
// the uart_bridge_send() call sites change.

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

// ---------------------------------------------------------------------------
// Message types: input events (BLE MIDI → mext)
// ---------------------------------------------------------------------------
#define UART_MSG_KEY_DOWN 0x01        // data: x(1) y(1)
#define UART_MSG_KEY_UP 0x02          // data: x(1) y(1)
#define UART_MSG_ENC_DELTA 0x03       // data: num(1) delta(1, int8 cast to uint8)
#define UART_MSG_ENC_SWITCH_DOWN 0x04 // data: num(1)
#define UART_MSG_ENC_SWITCH_UP 0x05   // data: num(1)
#define UART_MSG_TILT 0x06            // data: sensor(1) x(1) y(1) z(1)

// ---------------------------------------------------------------------------
// Message types: LED commands (norns → mext_grid/arc, unused in test variant)
// Kept for reference parity with uart_bridge.h.
// ---------------------------------------------------------------------------
#define UART_MSG_LED_ALL 0x10
#define UART_MSG_LED_MAP 0x11
#define UART_MSG_LED_INTENSITY 0x12
#define UART_MSG_LED_LEVEL_SET 0x13
#define UART_MSG_LED_LEVEL_ALL 0x14
#define UART_MSG_LED_LEVEL_MAP 0x15
#define UART_MSG_LED_LEVEL_ROW 0x16
#define UART_MSG_LED_LEVEL_COL 0x17
#define UART_MSG_RING_ALL 0x20
#define UART_MSG_RING_MAP 0x21
#define UART_MSG_RING_SET 0x22
#define UART_MSG_RING_RANGE 0x23
#define UART_MSG_RING_INTENSITY 0x24

#define UART_MSG_MAX_DATA 64

typedef struct
{
  uint8_t type;
  uint8_t len;
  uint8_t data[UART_MSG_MAX_DATA];
} uart_msg_t;

// Queue populated by ble_midi.c, consumed by event_router_task in main.c
extern QueueHandle_t g_event_rx_queue;

void event_queue_init(void);
