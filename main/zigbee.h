// Author: Felix Bengtsson

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initiera Zigbee-lagret.
 *
 * Just nu: bara logg + intern lamp-tabell (stub).
 * Senare: initiera riktig Zigbee-stack, starta nätverk osv.
 */
void zigbee_init(void);

/**
 * Tänd lampa med givet lamp_id.
 * lamp_id = 1, 2, ...
 */
void zigbee_light_on(uint8_t lamp_id);

/**
 * Släck lampa med givet lamp_id.
 */
void zigbee_light_off(uint8_t lamp_id);

/**
 * Sätt ljusstyrka för lampa med givet lamp_id.
 *
 * level = 0–255 (app-nivå). Internt kan detta mappas till Zigbee 0–254.
 */
void zigbee_light_set_brightness(uint8_t lamp_id, uint8_t level);

/**
 * Sätt färg i RGB för lampa med givet lamp_id.
 *
 * r, g, b = 0–255. Senare kan detta konverteras till Zigbee XY/CT.
 */
void zigbee_light_set_color_rgb(uint8_t lamp_id, uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif