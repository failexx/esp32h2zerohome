# ESP32-H2 Zigbee and BLE Bridge

This project is a Zigbee and BLE bridge developed using the [Espressif IoT Development Framework (ESP-IDF)](https://github.com/espressif/esp-idf). It is designed to run on the ESP32-H2 Zero development board.

## Features
- Zigbee communication using Espressif's ESP-Zigbee library.
- BLE communication using the ESP-IDF framework.
- Integration of Zigbee and BLE for seamless communication between devices.

## Hardware
- **Development Board**: ESP32-H2 Zero

## Setup Instructions
1. Clone this repository:
   ```bash
   git clone https://github.com/your-username/your-repo-name.git
   ```
2. Install the [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32h2/get-started/index.html) development environment.
3. Configure the project:
   ```bash
   idf.py set-target esp32h2
   idf.py menuconfig
   ```
4. Build and flash the project:
   ```bash
   idf.py build
   idf.py flash
   ```
5. Monitor the output:
   ```bash
   idf.py monitor
   ```

## File Structure
- `main/`: Contains the main application code, including BLE and Zigbee implementation.
- `build/`: Build artifacts (ignored in version control).
- `sdkconfig`: SDK configuration file (ignored in version control).
- `partitions.csv`: Partition table for the ESP32-H2.

## Dependencies
- [ESP-IDF](https://github.com/espressif/esp-idf)
- [ESP-Zigbee Library](https://github.com/espressif/esp-zigbee-lib)