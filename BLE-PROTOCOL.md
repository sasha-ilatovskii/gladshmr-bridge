# firmware-ble-raw — iPad Integration Guide

## Overview

`firmware-ble-raw` uses a **custom GATT service** instead of BLE MIDI. The device
is invisible to `Settings → Bluetooth` and to MIDI apps — it is only reachable from
your iPad app via CoreBluetooth, by scanning for the custom service UUID.

```
iPad app (CoreBluetooth)
  │
  │  Raw BLE — custom GATT service
  │  • Data characteristic    — key/encoder events (app → ESP32)
  │  • Config characteristic  — device layout     (app → ESP32)
  ▼
ESP32-S3
  │
  │  USB OTG (TinyUSB CDC-ACM composite)
  │  • CDC 0 → ttyACM0  (first configured device)
  │  • CDC 1 → ttyACM1  (second configured device)
  ▼
norns
```

---

## UUIDs

| Role | UUID | Properties |
|---|---|---|
| Service | `6D6F6E6F-6D65-0000-0000-000000000000` | — |
| Data characteristic | `6D6F6E6F-6D65-0000-0000-000000000001` | Write Without Response |
| Config characteristic | `6D6F6E6F-6D65-0000-0000-000000000002` | Write · Write Without Response |
| State characteristic | `6D6F6E6F-6D65-0000-0000-000000000003` | Notify |

Advertising name: **`monome-raw`**

```swift
let serviceUUID   = CBUUID(string: "6D6F6E6F-6D65-0000-0000-000000000000")
let dataCharUUID  = CBUUID(string: "6D6F6E6F-6D65-0000-0000-000000000001")
let cfgCharUUID   = CBUUID(string: "6D6F6E6F-6D65-0000-0000-000000000002")
let stateCharUUID = CBUUID(string: "6D6F6E6F-6D65-0000-0000-000000000003")
```

---

## Data Packet Format (app → ESP32)

Each write to the data characteristic is one complete event. No MIDI framing,
no timestamps. Packets are 2–3 bytes.

| Event | Type byte | Payload | Total |
|---|---|---|---|
| Grid key down | `0x10` | `x(1) y(1)` | 3 bytes |
| Grid key up | `0x11` | `x(1) y(1)` | 3 bytes |
| Arc encoder delta | `0x20` | `num(1) delta(1)` | 3 bytes |
| Arc encoder switch down | `0x21` | `num(1)` | 2 bytes |
| Arc encoder switch up | `0x22` | `num(1)` | 2 bytes |
| Grid tilt | `0x30` | `sensor(1) x_lo(1) x_hi(1) y_lo(1) y_hi(1) z_lo(1) z_hi(1)` | 8 bytes |

`x`, `y` are 0-based. `num` is encoder index 0–3. `delta` is a signed Int8
reinterpreted as UInt8 (e.g. +5 → `0x05`, −1 → `0xFF`).

`sensor` is the tilt sensor index (always `0` for a single grid). `x`/`y`/`z`
are signed 16-bit values in little-endian order. Scale is hardware-dependent;
typical range is −512 to +511 (10-bit ADC centred at zero).

```swift
// Grid key down at (x=3, y=2)
let packet: [UInt8] = [0x10, 3, 2]

// Arc encoder 0 delta +5
let delta: Int8 = 5
let packet: [UInt8] = [0x20, 0, UInt8(bitPattern: delta)]

// Arc encoder 0 delta -1
let delta: Int8 = -1
let packet: [UInt8] = [0x20, 0, UInt8(bitPattern: delta)]  // [0x20, 0x00, 0xFF]

// Grid tilt — sensor 0, x=100, y=-50, z=512
func sendTilt(sensor: UInt8, x: Int16, y: Int16, z: Int16) {
    let xb = UInt16(bitPattern: x)
    let yb = UInt16(bitPattern: y)
    let zb = UInt16(bitPattern: z)
    send([0x30, sensor,
          UInt8(xb & 0xFF), UInt8(xb >> 8),
          UInt8(yb & 0xFF), UInt8(yb >> 8),
          UInt8(zb & 0xFF), UInt8(zb >> 8)])
}
```

---

## Config Characteristic Format (app → ESP32)

Identical to `firmware-ble`. Write once after connecting, before sending events.

```
Byte 0:    slot_count  (0–2, hardware max 2 on ESP32-S3)
Byte 1:    slot[0].type    (1 = grid, 2 = arc)
Byte 2:    slot[0].param1  (grid: cols | arc: rings)
Byte 3:    slot[0].param2  (grid: rows | arc: 0)
Byte 4–6:  slot[1] (same layout)
```

```swift
// Grid 16×8 + Arc 4 rings
let config: [UInt8] = [0x02,  0x01, 16, 8,  0x02, 4, 0]

// Grid 16×8 only
let config: [UInt8] = [0x01,  0x01, 16, 8]

// Arc 4 only
let config: [UInt8] = [0x01,  0x02, 4, 0]
```

After writing config:
1. ESP32 saves to NVS and triggers USB soft-disconnect/reconnect (~250 ms)
2. norns re-enumerates and runs the mext handshake
3. Wait ~500 ms before sending events

---

## State Notifications (ESP32 → app)

When norns sends LED or ring commands to the ESP32 over CDC, they are forwarded
to the connected iPad as BLE notifications on the state characteristic.

**Packet format:** raw mext bytes — `[header_byte, payload...]`. No framing.

Subscribe immediately after discovering characteristics:

```swift
peripheral.setNotifyValue(true, for: stateChr)
```

Receive in the delegate:

```swift
func peripheral(_ peripheral: CBPeripheral,
                didUpdateValueFor characteristic: CBCharacteristic,
                error: Error?) {
    guard characteristic.uuid == stateCharUUID,
          let data = characteristic.value, !data.isEmpty else { return }
    let bytes = [UInt8](data)
    let opcode = bytes[0]
    let payload = Array(bytes.dropFirst())
    // decode opcode per table below
}
```

### Tilt enable/disable opcode

Norns enables and disables tilt via a single `TILT_SET` command forwarded to
the iPad as a state notification. This maps to `g:tilt_enable(sensor, value)`
in norns Lua.

| Opcode | Command | Payload |
|--------|---------|---------|
| `0x30` | TILT_SET | sensor(1) on_off(1) — `1` = enable, `0` = disable |

### Grid LED opcodes

| Opcode | Command | Payload |
|--------|---------|---------|
| `0x10` | LED_OFF | x(1) y(1) |
| `0x11` | LED_ON | x(1) y(1) |
| `0x12` | LED_ALL_OFF | — |
| `0x13` | LED_ALL_ON | — |
| `0x14` | LED_MAP | x_off(1) y_off(1) data[8] — bitmask |
| `0x15` | LED_ROW | x_off(1) y(1) data(1) |
| `0x16` | LED_COLUMN | x(1) y_off(1) data(1) |
| `0x17` | LED_INTENSITY | level(1) — 0–15 |
| `0x18` | LED_LEVEL_SET | x(1) y(1) level(1) |
| `0x19` | LED_LEVEL_ALL | level(1) |
| `0x1A` | LED_LEVEL_MAP | x_off(1) y_off(1) levels[32] — 64×4-bit packed |
| `0x1B` | LED_LEVEL_ROW | x_off(1) y(1) levels[4] — 8×4-bit packed |
| `0x1C` | LED_LEVEL_COLUMN | x(1) y_off(1) levels[4] |

### Arc ring opcodes

| Opcode | Command | Payload |
|--------|---------|---------|
| `0x90` | RING_SET | ring(1) led(1) level(1) |
| `0x91` | RING_ALL | ring(1) level(1) |
| `0x92` | RING_MAP | ring(1) levels[32] — 64×4-bit packed |
| `0x93` | RING_RANGE | ring(1) start(1) end(1) level(1) |
| `0x94` | RING_INTENSITY | ring(1) level(1) |

The largest packet (`LED_LEVEL_MAP`) is 35 bytes — well within the negotiated MTU.

---

## MTU Negotiation

The ESP32 requests MTU 247. iOS will negotiate on connect automatically — no app
code needed. Use `peripheral.maximumWriteValueLength(for: .withoutResponse)` to
confirm the usable payload size before sending large writes.

```swift
let mtu = peripheral.maximumWriteValueLength(for: .withoutResponse)
// Expect 182–244 bytes after negotiation on modern iOS
```

For key/encoder events (2–3 bytes) this is irrelevant. MTU matters if you later
add LED streaming (256-byte frames fit in one or two packets at full MTU).

---

## Swift Integration

### 1. Setup

```swift
import CoreBluetooth

class MonomeRawBLE: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var dataChr: CBCharacteristic?
    private var cfgChr: CBCharacteristic?

    private let serviceUUID   = CBUUID(string: "6D6F6E6F-6D65-0000-0000-000000000000")
    private let dataCharUUID  = CBUUID(string: "6D6F6E6F-6D65-0000-0000-000000000001")
    private let cfgCharUUID   = CBUUID(string: "6D6F6E6F-6D65-0000-0000-000000000002")
    private let stateCharUUID = CBUUID(string: "6D6F6E6F-6D65-0000-0000-000000000003")

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }
}
```

### 2. Scan

Scan by service UUID — this is the only reliable way to find the device.
It also works when the app is backgrounded (see Background section below).

```swift
func centralManagerDidUpdateState(_ central: CBCentralManager) {
    if central.state == .poweredOn {
        central.scanForPeripherals(withServices: [serviceUUID])
    }
}
```

### 3. Connect

```swift
func centralManager(_ central: CBCentralManager,
                    didDiscover peripheral: CBPeripheral,
                    advertisementData: [String: Any],
                    rssi RSSI: NSNumber) {
    self.peripheral = peripheral
    central.stopScan()
    central.connect(peripheral)
}

func centralManager(_ central: CBCentralManager,
                    didConnect peripheral: CBPeripheral) {
    peripheral.delegate = self
    peripheral.discoverServices([serviceUUID])
}
```

### 4. Discover characteristics

```swift
func peripheral(_ peripheral: CBPeripheral,
                didDiscoverServices error: Error?) {
    guard let service = peripheral.services?.first(where: { $0.uuid == serviceUUID })
    else { return }
    peripheral.discoverCharacteristics([dataCharUUID, cfgCharUUID], for: service)
}

func peripheral(_ peripheral: CBPeripheral,
                didDiscoverCharacteristicsFor service: CBService,
                error: Error?) {
    for chr in service.characteristics ?? [] {
        if chr.uuid == dataCharUUID  { dataChr  = chr }
        if chr.uuid == cfgCharUUID   { cfgChr   = chr }
        if chr.uuid == stateCharUUID { stateChr = chr }
    }
    // Subscribe to LED/ring state notifications before sending config
    if let s = stateChr { peripheral.setNotifyValue(true, for: s) }
    sendConfig()
}
```

### 5. Send config

```swift
func sendConfig() {
    guard let chr = cfgChr else { return }
    let config: [UInt8] = [0x02,  0x01, 16, 8,  0x02, 4, 0]  // grid16×8 + arc4
    peripheral?.writeValue(Data(config), for: chr, type: .withoutResponse)

    // Wait for norns USB re-enumeration
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
        self.readyToSendEvents = true
    }
}
```

### 6. Send events

```swift
func sendKeyDown(x: UInt8, y: UInt8) {
    send([0x10, x, y])
}

func sendKeyUp(x: UInt8, y: UInt8) {
    send([0x11, x, y])
}

func sendEncDelta(num: UInt8, delta: Int8) {
    send([0x20, num, UInt8(bitPattern: delta)])
}

func sendEncSwitchDown(num: UInt8) { send([0x21, num]) }
func sendEncSwitchUp(num: UInt8)   { send([0x22, num]) }

private func send(_ bytes: [UInt8]) {
    guard let p = peripheral, let chr = dataChr, readyToSendEvents else { return }
    p.writeValue(Data(bytes), for: chr, type: .withoutResponse)
}
```

### 7. Receive state notifications

```swift
func peripheral(_ peripheral: CBPeripheral,
                didUpdateValueFor characteristic: CBCharacteristic,
                error: Error?) {
    guard characteristic.uuid == stateCharUUID,
          let data = characteristic.value, !data.isEmpty else { return }
    let bytes = [UInt8](data)
    let opcode = bytes[0]
    let payload = Array(bytes.dropFirst())
    handleLEDState(opcode: opcode, payload: payload)
}
```

### 8. Handle disconnect

```swift
func centralManager(_ central: CBCentralManager,
                    didDisconnectPeripheral peripheral: CBPeripheral,
                    error: Error?) {
    dataChr  = nil
    cfgChr   = nil
    stateChr = nil
    readyToSendEvents = false
    // Re-scan
    central.scanForPeripherals(withServices: [serviceUUID])
}
```

---

## Recommended Connect Flow

```
1. CBCentralManager state → .poweredOn
2. scanForPeripherals(withServices: [serviceUUID])
3. didDiscover → connect
4. didConnect → discoverServices
5. didDiscoverCharacteristics → subscribe to state characteristic (setNotifyValue true)
6. didDiscoverCharacteristics → write config characteristic
7. wait 500 ms
8. begin sending event packets to data characteristic
9. receive LED/ring state via didUpdateValueFor notifications
```

---

## Info.plist Requirements

```xml
<!-- Required for CoreBluetooth -->
<key>NSBluetoothAlwaysUsageDescription</key>
<string>Used to connect to the monome controller</string>

<!-- If the app needs to scan in the background -->
<key>UIBackgroundModes</key>
<array>
    <string>bluetooth-central</string>
</array>
```

---

## Background Scanning

iOS allows background BLE scanning only when `withServices:` is specified (not nil).
The scan above uses `[serviceUUID]`, so it works in the background. The system may
deliver `didDiscover` with a delay of several seconds when backgrounded — this is
normal.

---

## Compared to firmware-ble (BLE MIDI)

| | `firmware-ble` | `firmware-ble-raw` |
|---|---|---|
| Discovery | CoreMIDI / MIDI apps | CoreBluetooth only |
| Scan filter | BLE MIDI service UUID | Custom service UUID |
| Packet format | BLE MIDI (header + timestamp + MIDI) | Raw binary (2–3 bytes) |
| Config write | Same format | Same format |
| Notify subscription | Required by BLE MIDI spec | Not needed |
| MTU | Default 20 bytes (iOS) | Negotiated to 244 bytes |
| 2M PHY | No | Requested on connect |
