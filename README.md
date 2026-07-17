# ESP32-H2 Zigbee and BLE Bridge

A bridge application for the ESP32-H2 Zero that receives BLE commands and translates them into Zigbee light control actions.

This project is built with the [Espressif IoT Development Framework (ESP-IDF)](https://github.com/espressif/esp-idf) and uses the Espressif Zigbee library.

## Current status
- BLE → Zigbee bridge logic is implemented.
- `ON` / `OFF` commands work.
- Brightness control works.
- RGB color commands are implemented in code but not fully working yet.

## Features
- BLE communication using ESP-IDF.
- Zigbee control using Espressif's ESP-Zigbee library.
- Bridge logic for translating BLE packets into Zigbee light commands.

## Hardware
- **Development Board**: ESP32-H2 Zero

## Setup Instructions
1. Clone this repository:
   ```bash
   git clone https://github.com/failexx/esp32-zigbee-ble-bridge.git
   ```
2. Install the [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32h2/get-started/index.html) development environment.
3. Set the target and configure the project:
   ```bash
   idf.py set-target esp32h2
   idf.py menuconfig
   ```
4. Build the firmware:
   ```bash
   idf.py build
   ```
5. Flash the board:
   ```bash
   idf.py -p /dev/ttyUSB0 flash
   ```
6. Monitor serial output:
   ```bash
   idf.py monitor
   ```

## Notes
- `build/` and generated SDK configuration files should not be committed.
- `partitions.csv` is the partition table used for the board.

## File structure
- `main/`: Main application code with BLE and Zigbee logic.
- `build/`: Build artifacts (ignored in version control).
- `sdkconfig`: SDK configuration file (ignored in version control).
- `partitions.csv`: Partition table for ESP32-H2.

## Dependencies
- [ESP-IDF](https://github.com/espressif/esp-idf)
- [ESP-Zigbee Library](https://github.com/espressif/esp-zigbee-lib)
- [Apache NimBLE](https://github.com/apache/mynewt-nimble)
- [FreeRTOS](https://www.freertos.org/)

## Credits
- **ESP-IDF**: Espressif IoT Development Framework.
- **ESP-Zigbee Library**: Zigbee support from Espressif.
- **Apache NimBLE**: BLE stack implementation.
- **FreeRTOS**: Real-time operating system used by ESP-IDF.

Special thanks to the open-source community for these tools and libraries.
