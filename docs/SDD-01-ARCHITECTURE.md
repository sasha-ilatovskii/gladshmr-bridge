# SDD-01: Architecture & Concurrency Model

## System Context
The `firmware-ble-raw` application is a bridging firmware for an ESP32-S3 microcontroller. It serves a dual role:
1. **BLE Peripheral:** It advertises a custom GATT service to which an iPad connects (via CoreBluetooth).
2. **USB CDC-ACM Device:** It acts as a composite USB device to a host (specifically the monome norns), presenting dynamic virtual serial ports (`ttyACM0`, `ttyACM1`) that emulate physical monome grid and arc devices.

This allows the iPad app to define virtual devices and send user interactions (key presses, encoder turns) over BLE, which the ESP32 then translates and forwards over USB as standard monome `mext` protocol messages. Conversely, LED and ring updates from the norns are sent over USB, intercepted by the ESP32, and pushed to the iPad over BLE notifications.

## Data Flow
```text
iPad App (CoreBluetooth)
      │
      │  Raw BLE (Custom GATT Service)
      │  • Data Char (Write Without Response): Input events (App → ESP32)
      │  • Config Char (Write): Device layout (App → ESP32)
      ▼
ESP32-S3 (NimBLE + TinyUSB)
      │
      │  Event Router (g_event_rx_queue)
      │
      ├─ Slot 0 Queue ──▶ Slot 0 Task ──▶ CDC 0 (ttyACM0)
      └─ Slot 1 Queue ──▶ Slot 1 Task ──▶ CDC 1 (ttyACM1)
                                                │
                                                ▼
                                            monome norns (USB Host)
```

## RTOS Concurrency Model
The firmware utilizes FreeRTOS tasks to manage asynchronous I/O and protocol translation:
1. **`main` task:** Initializes hardware (NVS, BLE, USB) and spawns other tasks.
2. **`usb_device_task`:** A dedicated high-priority task running `tud_task()` every 1ms to service TinyUSB interrupts and endpoints.
3. **`event_router_task`:** Consumes raw BLE input events from `g_event_rx_queue` (populated by `ble_raw.c`) and dispatches them to the appropriate slot-specific queue using `mext_slot_dispatch()`.
4. **Slot Tasks (`mext_slot0`, `mext_slot1`, etc.):** Created by `mext_slot_init()`. Each task manages a single CDC interface. It drains its specific event queue to send data to the norns and continuously polls `tud_cdc_n_read()` to receive and parse incoming LED/ring commands from the norns.

## Hardware & Configuration Constraints
*   **USB Endpoints (ESP32-S3):** The ESP32-S3 hardware has a limit of 5 usable IN endpoints (EP1-EP5). Each CDC interface requires 2 IN endpoints (one for interrupt/notify, one for data) and 1 OUT endpoint.
*   **Max Slots:** Because of the IN endpoint limit, the practical maximum number of active monome devices (CDC slots) is **2**.
*   **TinyUSB Config (`tusb_config.h`):** The `CFG_TUD_CDC` macro is strictly set to `2`. Attempting to configure or allocate more than 2 slots will fail USB enumeration or cause undefined behavior. The `MAX_MONOME_SLOTS` in `monome_slots.h` is set to `4` for future flexibility (e.g., if ported to hardware with more endpoints), but only 2 can be actively mapped to USB.

## Dead Code / Legacy Files
The following files are present in the directory but are **not** compiled (excluded from `CMakeLists.txt`) and should be ignored or deleted by agents:
*   `device_config.c` / `device_config.h` (superseded by `monome_slots`)
*   `mext_grid.c` / `mext_grid.h` (superseded by unified `mext_slot`)
*   `mext_arc.c` / `mext_arc.h` (superseded by unified `mext_slot`)
