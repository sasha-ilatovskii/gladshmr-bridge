# SDD-03: USB MEXT Routing & Handshake

## Dynamic Descriptors (`usb_descriptors.c`)
The `firmware-ble-raw` uses dynamic TinyUSB descriptors.
*   The `g_slot_count` (managed by `monome_slots.c`) determines how many CDC interfaces are instantiated.
*   `usb_desc_build(int n_slots)` allocates the string index and endpoint addresses linearly based on the number of slots.
*   The max usable `n_slots` is strictly bound to 2 due to ESP32-S3 `IN` endpoint limits and the `CFG_TUD_CDC` macro.

## MEXT Protocol Parsing (`mext_slot.c`)
Each virtual device handles its own USB CDC logic via an RTOS task (`slot_task`) checking `tud_cdc_n_connected()` and `tud_cdc_n_read()`.

### The Handshake Interception
When the norns connects to the virtual serial port, it queries the device type and ID using the `mext` protocol. The ESP32 intercepts these 0-payload queries locally and responds immediately, simulating a physical grid or arc. The iPad is **not** involved in the handshake.

*   `MEXT_SYSTEM_QUERY`:
    *   Grid: Responds with `SS_KEY_GRID` and `SS_TILT` capabilities.
    *   Arc: Responds with `SS_ENCODER` capability.
*   `MEXT_SYSTEM_GET_ID`:
    *   Grid: Responds with a formatted string `m...`
    *   Arc: Responds with a formatted string `a...`
*   `MEXT_SYSTEM_GET_GRIDSZ`:
    *   Grid: Uses `g_slots[slot_idx].param1` (cols) and `param2` (rows) to formulate the grid dimension response.

### Payload Interception
When `mext_slot.c` receives commands that contain payloads (e.g., LED mapping `0x1A` or Arc Ring level `0x92`), the state machine progresses from `ST_HEADER` to `ST_PAYLOAD` based on the known lengths mapped in `MEXT_INCOMING_PAYLOAD_LEN`.

Once the payload is completely received, the entire packet `[header_byte, payload...]` is dispatched to the iPad via `ble_raw_send_state()`.

## Event Dispatch (iPad → norns)
1.  The iPad app pushes events via the BLE Data Characteristic.
2.  `ble_raw.c` pushes a `uart_msg_t` struct to `g_event_rx_queue`.
3.  `event_router_task` pops the queue and uses `mext_slot_dispatch()` to route the message.
    *   `KEY_DOWN`, `KEY_UP`, `TILT` are routed to the first configured `MONOME_SLOT_GRID`.
    *   `ENC_DELTA`, `ENC_SWITCH` are routed to the first configured `MONOME_SLOT_ARC`.
4.  The specific slot's RTOS task pulls from its personal queue and writes the formatted `mext` byte string (e.g., `MEXT_KEY_DOWN`, `x`, `y`) into `tud_cdc_n_write()`.

### Tilt Compatibility Note
`libmonome`'s `mext` tilt payload is `[sensor, x, y, z]`, but norns' Lua `grid.tilt` wrapper exposes only `g.tilt(x, y, z)`. The wrapper receives the sensor as its first axis and drops the final payload value, so `mext_slot.c` deliberately packs BLE tilt axes into the first three values that norns forwards to Lua.
