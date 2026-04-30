// Author: Felix Bengtsson

#include "commands.h"
#include "zigbee.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "COMMANDS";

void commands_init(void)
{
    ESP_LOGI(TAG, "commands_init() called");
}

// Handle hex commands from BLE
bool commands_handle_hex(const uint8_t *data, uint16_t len)
{
    if (len < 2) {
        ESP_LOGW(TAG, "HEX cmd too short, len=%u", len);
        return false;
    }

    uint8_t lamp_id = data[0];
    uint8_t cmd     = data[1];

    ESP_LOGI(TAG, "HEX cmd: lamp_id=0x%02X, cmd=0x%02X, len=%u", lamp_id, cmd, len);

    switch (cmd) {
    case 0x00: // OFF
        ESP_LOGI(TAG, "Command = OFF for lamp %u", lamp_id);
        zigbee_light_off(lamp_id);
        return true;

    case 0x01: // ON
        ESP_LOGI(TAG, "Command = ON for lamp %u", lamp_id);
        zigbee_light_on(lamp_id);
        return true;

    case 0x02: // BRIGHTNESS
        if (len < 3) {
            ESP_LOGW(TAG, "BRIGHTNESS cmd too short, need 3 bytes");
            return false;
        } else {
            uint8_t level = data[2];
            ESP_LOGI(TAG, "BRIGHTNESS for lamp %u: level=%u", lamp_id, level);
            zigbee_light_set_brightness(lamp_id, level);
            return true;
        }

    case 0x03: // COLOR RGB
        if (len < 5) {
            ESP_LOGW(TAG, "COLOR_RGB cmd too short, need 5 bytes");
            return false;
        } else {
            uint8_t r = data[2];
            uint8_t g = data[3];
            uint8_t b = data[4];
            ESP_LOGI(TAG, "COLOR_RGB for lamp %u: R=%u, G=%u, B=%u",
                     lamp_id, r, g, b);
            zigbee_light_set_color_rgb(lamp_id, r, g, b);
            return true;
        }

    default:
        ESP_LOGW(TAG, "Unknown command 0x%02X for lamp %u", cmd, lamp_id);
        return false;
    }
}