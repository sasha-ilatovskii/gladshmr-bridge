# SDD-04: State, Configuration & Persistence

## Configuration Structure
The iPad app dictates the layout of the virtual monome devices through the BLE Config Characteristic.

**Config Packet Payload:**
*   `Byte 0`: `slot_count` (0–2, hardware max 2)
*   `Byte 1`: `slot[0].type` (1 = `MONOME_SLOT_GRID`, 2 = `MONOME_SLOT_ARC`)
*   `Byte 2`: `slot[0].param1` (grid: cols | arc: rings)
*   `Byte 3`: `slot[0].param2` (grid: rows | arc: 0)
*   `Byte 4–6`: `slot[1]` (Same layout as bytes 1-3)

## NVS Persistence (`monome_slots.c`)
*   `monome_slots_init()`: Runs on boot. Attempts to load `monome_slots` and `slot_count` from NVS under the `nvs` namespace. If missing, it defaults to a 16x8 Grid on Slot 0 and a 4-ring Arc on Slot 1.
*   `monome_slots_apply()`:
    1.  Receives the updated configuration array from `ble_raw_config_cb`.
    2.  Updates `g_slots` and `g_slot_count` in RAM.
    3.  Saves the binary struct to NVS so the configuration survives reboot.
    4.  Triggers soft USB re-enumeration.

## Soft USB Re-enumeration
Because TinyUSB descriptors must be static upon physical connection to the host OS, changing the number of CDC interfaces requires forcing the USB host to physically re-evaluate the device.

When `monome_slots_apply()` completes:
1.  It calls `usb_desc_build(g_slot_count)` to regenerate the endpoints strings.
2.  It creates a transient FreeRTOS task: `usb_reenum_task`.
3.  The task forcefully disconnects the TinyUSB D+ pull-up via `usb_phy_action(USB_PHY_ACTION_HOST_FORCE_DISCONNECT)`.
4.  The task delays for 250ms. This pause is critical to ensure the Linux/macOS/norns kernel realizes the device has detached and cleans up the `ttyACM` handles.
5.  The task reconnects the pull-up via `usb_phy_action(USB_PHY_ACTION_HOST_ALLOW_CONN)`.
6.  The norns detects a "new" USB device, pulls the updated dynamic descriptors, and begins the `mext` handshake for the new layout.
