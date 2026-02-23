// Author: Felix Bengtsson

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void commands_init(void);

// If you want to keep text commands:
bool commands_handle(const char *cmd, uint16_t len);

// Hex commands from BLE
bool commands_handle_hex(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif