// This file is part of the ESP32-H2 Zigbee and BLE bridge project.
// Developed using the Espressif IoT Development Framework (ESP-IDF).
// Author: Felix Bengtsson

#include "ble-bridge.h"
#include "zigbee.h"
#include "commands.h"

void app_main(void)
{
    ESP_LOGI("MAIN", "Starting ESP32-H2 Zigbee and BLE bridge project");
    commands_init();
    zigbee_init();
    ble_bridge_init();
}