#pragma once
#include "tusb.h"
#include <stdint.h>

// USB string descriptor indices
enum
{
  STRID_LANGID = 0,
  STRID_MANUF = 1,   // "monome"
  STRID_PRODUCT = 2, // "monome"
  STRID_SERIAL = 3,  // "m1000001"
  STRID_COUNT
};

extern tusb_desc_device_t const desc_device;

// Build the USB configuration descriptor into an internal RAM buffer
// for n_slots active CDC-ACM interfaces. Must be called before tusb_init()
// at startup, and before tud_connect() whenever the slot count changes.
void usb_desc_build(int n_slots);
