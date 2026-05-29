#pragma once
// Monome device slot configuration.
//
// Each slot maps to one CDC-ACM interface on the USB OTG port.
// Slot i → CDC interface i → /dev/ttyACMi on norns.
//
// Slot layout is persisted in NVS and survives power cycles.
// When the iPad app changes the layout, monome_slots_apply() saves
// the new config and triggers a USB re-enumeration (soft disconnect +
// reconnect) so norns sees the updated set of ttyACM devices.
//
// Hardware note: ESP32-S3 has 5 usable IN endpoints (EP1-EP5).
// Each CDC slot needs 2 IN (notify + data) + 1 OUT, so practical
// max is 2 active slots. MAX_MONOME_SLOTS is 4 for future flexibility.

#include <stdint.h>

typedef enum
{
  MONOME_SLOT_DISABLED = 0,
  MONOME_SLOT_GRID = 1,
  MONOME_SLOT_ARC = 2,
} monome_slot_type_t;

typedef struct
{
  monome_slot_type_t type;
  uint8_t param1; // grid: cols   | arc: rings
  uint8_t param2; // grid: rows   | arc: unused (0)
} monome_slot_t;

#define MAX_MONOME_SLOTS 4

// Active slot array. Slot i maps directly to CDC interface i.
// Only [0 .. g_slot_count-1] are non-DISABLED.
extern monome_slot_t g_slots[MAX_MONOME_SLOTS];
extern int g_slot_count;

// Load slot config from NVS. Falls back to default (grid16x8 + arc4)
// if no saved config exists. Call after nvs_flash_init().
void monome_slots_init(void);

// Apply a new slot layout:
//   1. Updates g_slots / g_slot_count in RAM
//   2. Persists to NVS
//   3. Rebuilds the USB configuration descriptor
//   4. Triggers soft USB re-enumeration (norns sees new ttyACM set)
// Safe to call from any task context — re-enum runs in a short-lived task.
void monome_slots_apply(const monome_slot_t* slots, int count);
