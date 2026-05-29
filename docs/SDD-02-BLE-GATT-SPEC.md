# SDD-02: BLE GATT Specification

## BLE Stack Configuration
The firmware uses the **NimBLE** stack configured strictly as a **Peripheral**.
*   **Connections:** Max 1 connection (only one iPad can control the devices).
*   **Advertising:** Advertises indefinitely (`BLE_HS_FOREVER`) as `"monome-raw"`. It uses general discoverable mode but disables BR/EDR.
*   **MTU:** Requests a preferred ATT MTU of `247` bytes upon connection. iOS will auto-negotiate. This allows large LED/ring state payloads to be sent in single notifications.

## Custom Service and UUIDs
The system abandons the standard BLE MIDI service in favor of a custom, highly optimized binary protocol to eliminate MIDI framing overhead and timestamps.

**Base UUID Pattern:** `6D6F6E6F-6D65-0000-0000-00000000000X` (Encodes "monome")

| Role | UUID | Properties | Direction |
| :--- | :--- | :--- | :--- |
| **Service** | `...0000` | - | - |
| **Data Char** | `...0001` | Write Without Response | iPad → ESP32 |
| **Config Char** | `...0002` | Write, Write Without Response | iPad → ESP32 |
| **State Char** | `...0003` | Notify | ESP32 → iPad |

## Data Characteristic (App → ESP32)
Used for real-time interaction events (keys, encoders, tilt). Packets are 2-8 bytes long. Written directly to the characteristic without responses to minimize latency.

| Event | Opcode | Payload Format | Total Bytes |
| :--- | :--- | :--- | :--- |
| Grid Key Down | `0x10` | `x(1)` `y(1)` | 3 |
| Grid Key Up | `0x11` | `x(1)` `y(1)` | 3 |
| Arc Enc Delta | `0x20` | `num(1)` `delta(1)` (signed 8-bit) | 3 |
| Arc Enc SW Down | `0x21` | `num(1)` | 2 |
| Arc Enc SW Up | `0x22` | `num(1)` | 2 |
| Grid Tilt | `0x30` | `sensor(1)` `x(2,LE)` `y(2,LE)` `z(2,LE)` | 8 |

*Implementation Note:* These are pushed into `g_event_rx_queue` by NimBLE callbacks in `ble_raw.c`.

## Config Characteristic (App → ESP32)
Used to define the layout of virtual devices. See `SDD-04-STATE-AND-CONFIG.md` for payload structure and processing logic.

## State Characteristic (ESP32 → App)
Used to forward LED and Ring commands from the norns back to the iPad for rendering.
*   Requires the iPad app to subscribe to Notifications (`setNotifyValue:true`).
*   **Payload Format:** Raw `mext` protocol bytes `[header_byte, payload...]`. No framing or modification is applied by the ESP32.
*   **Routing:** When `mext_slot.c` successfully parses a complete `mext` packet from the USB CDC interface (state machine reaches `ST_HEADER` after `ST_PAYLOAD`), it immediately calls `ble_raw_send_state()` to notify the iPad.
*   Max packet size is 35 bytes (`LED_LEVEL_MAP`), which easily fits within the negotiated 247-byte MTU.
