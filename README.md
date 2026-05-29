# gladshmr-bridge

ESP32 firmware for iPad integration with norns over USB CDC.

This repository contains the source code for an ESP32-S3 firmware image that exposes a custom BLE GATT service named `monome-raw`, receives raw grid/arc/touch events from an iPad app, and forwards device state updates back to the app over BLE.

## Features

- Custom BLE service for raw device events
- USB CDC-ACM composite device support for norns integration
- iPad-compatible raw BLE packet formats and state notifications
- Configurable grid/arc layout via BLE config characteristic
- Documentation and design notes in `BLE-PROTOCOL.md` and `docs/`

## Repository contents

- `CMakeLists.txt` — ESP-IDF project entry point
- `main/` — firmware source code and component manifest (`idf_component.yml`)
- `docs/` — design and architecture documentation
- `BLE-PROTOCOL.md` — BLE GATT protocol reference and packet formats
- `sdkconfig`, `sdkconfig.defaults` — build configuration

**Note:** External dependencies (e.g., TinyUSB) are specified in `main/idf_component.yml` and automatically installed into `managed_components/` by `idf.py` during the build process.

## Building

### Requirements

- ESP-IDF installed and configured
- `IDF_PATH` environment variable set
- ESP32-S3 toolchain installed

### Build steps

1. Clone the repository and enter the directory:
   ```bash
   git clone https://github.com/sasha-ilatovskii/gladshmr-bridge.git
   cd gladshmr-bridge
   ```

2. Configure and build:
   ```bash
   idf.py set-target esp32s3
   idf.py build
   ```
   ESP-IDF will automatically fetch and install required components from `idf_component.yml` into `managed_components/`.

### Flashing

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Replace `/dev/ttyUSB0` with the actual serial port for your ESP32 device.

## Notes

- The firmware uses a custom service UUID and is not visible as a generic BLE MIDI device.
- `BLE-PROTOCOL.md` documents the packet format, UUIDs, and protocol details for iPad integration.
- The `docs/` folder contains detailed design and architecture documentation.

## License

This project is licensed under the MIT License. See `LICENSE`.
