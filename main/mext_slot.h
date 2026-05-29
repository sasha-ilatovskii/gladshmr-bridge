#pragma once
// Unified mext protocol handler — one task per CDC slot.
//
// Replaces the separate mext_grid / mext_arc pair. Each task reads its
// slot type from g_slots[slot_index] and responds to the mext handshake
// accordingly (grid or arc). Tasks are created for all MAX_MONOME_SLOTS
// slots at startup; disabled slots just sleep until activated.

#include "event_queue.h"

// Start one CDC task per slot (up to MAX_MONOME_SLOTS tasks total).
void mext_slot_init(void);

// Route an input event (from BLE MIDI) to the correct slot's queue.
// Finds the first slot whose type matches the event type:
//   KEY events   → first MONOME_SLOT_GRID slot
//   ENC events   → first MONOME_SLOT_ARC  slot
void mext_slot_dispatch(const uart_msg_t* msg);
