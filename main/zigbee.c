// Author: Felix Bengtsson

// This file is part of the ESP32-H2 Zigbee and BLE bridge project.
// Developed using the Espressif IoT Development Framework (ESP-IDF).
// Zigbee functionality is implemented using the ESP-Zigbee library.

#include "zigbee.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee_core.h"
#include "platform/esp_zigbee_platform.h"
#include "bdb/esp_zigbee_bdb_commissioning.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zcl/esp_zigbee_zcl_on_off.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zdo/esp_zigbee_zdo_common.h"
#include "zdo/esp_zigbee_zdo_command.h"

#include <string.h>
#include <stdbool.h>

static void zigbee_device_join_cb(const esp_zb_zdo_signal_device_annce_params_t *join, esp_zb_app_signal_t *signal_struct);
static void zigbee_stack_task(void *arg);
static void zigbee_retry_formation(uint8_t unused);
static void zigbee_retry_steering(uint8_t unused);
static void zigbee_enter_search_mode(void);
static void rgb_to_hue_saturation(uint8_t r, uint8_t g, uint8_t b, uint8_t *hue, uint8_t *saturation);

static const char *TAG = "ZIGBEE";

#define ZIGBEE_SWITCH_ENDPOINT 1
#define ZIGBEE_TASK_STACK_SIZE 6144
#define ZIGBEE_TASK_PRIO       5
#define JOIN_WINDOW_SECONDS    180
#define MAX_LAMPS              1
#define REQUIRED_LAMPS         1

static bool g_zigbee_started = false;
static bool g_network_ready = false;
static bool g_open_join_requested = false;

static void zigbee_retry_formation(uint8_t unused)
{
    (void)unused;
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Retry formation failed to start, err=%d", err);
    }
}

static void zigbee_retry_steering(uint8_t unused)
{
    (void)unused;
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Retry steering failed to start, err=%d", err);
    }
}

static void zigbee_enter_search_mode(void)
{
    if (!g_network_ready) {
        ESP_LOGW(TAG, "Cannot enter search mode: device not in Zigbee network yet");
        return;
    }

    esp_err_t open_err = esp_zb_bdb_open_network(JOIN_WINDOW_SECONDS);
    if (open_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open network for joining, err=%d", open_err);
    } else {
        ESP_LOGI(TAG,
                 "SEARCH MODE: permit-join open for %us; waiting for %d real lamp",
                 JOIN_WINDOW_SECONDS,
                 REQUIRED_LAMPS);
    }
}

/**
 * Internal representation of a Zigbee lamp.
 *
 * Later, short_addr/endpoint can be set to actual values when the lamp joins the network.
 */
typedef struct {
    uint8_t  lamp_id;       // 1, 2, ...
    uint16_t short_addr;    // Zigbee short address (0x0000–0xFFFE)
    uint8_t  endpoint;      // Zigbee endpoint (often 1)
    esp_zb_ieee_addr_t ieee_addr; // unique IEEE-address for stable mapping when rejoin
    bool     joined;        // true if the lamp is known in the network
    char     name[16];      // logging/debug name
} zigbee_lamp_t;

static esp_err_t zigbee_send_onoff(zigbee_lamp_t *lamp, uint8_t command_id);
static esp_err_t zigbee_send_level(zigbee_lamp_t *lamp, uint8_t level);
static esp_err_t zigbee_send_color_hs(zigbee_lamp_t *lamp, uint8_t hue, uint8_t saturation);

// TODO: When you know the actual Zigbee addresses, you can fill in short_addr/endpoint
// and optionally set joined = true only after the lamp has joined the network.
static zigbee_lamp_t g_lamps[MAX_LAMPS] = {
    { .lamp_id = 1, .short_addr = 0x0000, .endpoint = 1, .ieee_addr = {0}, .joined = false, .name = "Lamp1" },
};

static void zigbee_reset_lamp_state(void)
{
    for (int i = 0; i < MAX_LAMPS; ++i) {
        g_lamps[i].short_addr = 0x0000;
        memset(g_lamps[i].ieee_addr, 0, sizeof(esp_zb_ieee_addr_t));
        g_lamps[i].joined = false;
    }
}

static bool zigbee_ieee_is_zero(const esp_zb_ieee_addr_t ieee_addr)
{
    for (size_t i = 0; i < sizeof(esp_zb_ieee_addr_t); ++i) {
        if (ieee_addr[i] != 0) {
            return false;
        }
    }
    return true;
}

static int zigbee_joined_lamp_count(void)
{
    int count = 0;
    for (int i = 0; i < MAX_LAMPS; ++i) {
        if (g_lamps[i].joined) {
            ++count;
        }
    }
    return count;
}

static zigbee_lamp_t *zigbee_find_lamp(uint8_t lamp_id)
{
    for (int i = 0; i < MAX_LAMPS; ++i) {
        if (g_lamps[i].lamp_id == lamp_id) {
            return &g_lamps[i];
        }
    }
    return NULL;
}

static zigbee_lamp_t *zigbee_find_lamp_by_ieee(const esp_zb_ieee_addr_t ieee_addr)
{
    for (int i = 0; i < MAX_LAMPS; ++i) {
        if (memcmp(g_lamps[i].ieee_addr, ieee_addr, sizeof(esp_zb_ieee_addr_t)) == 0) {
            return &g_lamps[i];
        }
    }
    return NULL;
}

static zigbee_lamp_t *zigbee_find_first_free_lamp(void)
{
    for (int i = 0; i < MAX_LAMPS; ++i) {
        if (!g_lamps[i].joined) {
            return &g_lamps[i];
        }
    }
    return NULL;
}

/**
 * Helper function: fetch lamp pointer or log a warning.
 */
static zigbee_lamp_t *require_lamp(uint8_t lamp_id)
{
    zigbee_lamp_t *lamp = zigbee_find_lamp(lamp_id);
    if (!lamp) {
        ESP_LOGW(TAG, "lamp_id=%u not found", lamp_id);
        return NULL;
    }
    if (!lamp->joined) {
        ESP_LOGW(TAG, "lamp_id=%u (%s) not marked as joined, ignoring command",
                 lamp->lamp_id, lamp->name);
        return NULL;
    }
    return lamp;
}

void zigbee_init(void)
{
    ESP_LOGI(TAG, "zigbee_init() called");

    zigbee_reset_lamp_state();

    ESP_LOGI(TAG, "Configured lamps: %d", MAX_LAMPS);
    for (int i = 0; i < MAX_LAMPS; ++i) {
        ESP_LOGI(TAG,
                 "  lamp_id=%u name=%s short_addr=0x%04X ep=%u joined=%d",
                 g_lamps[i].lamp_id,
                 g_lamps[i].name,
                 g_lamps[i].short_addr,
                 g_lamps[i].endpoint,
                 g_lamps[i].joined);
    }

    BaseType_t task_ok = xTaskCreate(zigbee_stack_task,
                                     "zb_stack",
                                     ZIGBEE_TASK_STACK_SIZE,
                                     NULL,
                                     ZIGBEE_TASK_PRIO,
                                     NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Zigbee task");
    }
}

void zigbee_light_on(uint8_t lamp_id)
{
    zigbee_lamp_t *lamp = require_lamp(lamp_id);
    if (!lamp) {
        return;
    }

    ESP_LOGI(TAG, "zigbee_light_on(lamp_id=%u, addr=0x%04X, ep=%u)",
             lamp->lamp_id, lamp->short_addr, lamp->endpoint);

    esp_err_t err = zigbee_send_onoff(lamp, ESP_ZB_ZCL_CMD_ON_OFF_ON_ID);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send ON to lamp_id=%u, err=%d", lamp->lamp_id, err);
    }
}

void zigbee_light_off(uint8_t lamp_id)
{
    zigbee_lamp_t *lamp = require_lamp(lamp_id);
    if (!lamp) {
        return;
    }

    ESP_LOGI(TAG, "zigbee_light_off(lamp_id=%u, addr=0x%04X, ep=%u)",
             lamp->lamp_id, lamp->short_addr, lamp->endpoint);

    esp_err_t err = zigbee_send_onoff(lamp, ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send OFF to lamp_id=%u, err=%d", lamp->lamp_id, err);
    }
}

void zigbee_light_set_brightness(uint8_t lamp_id, uint8_t level)
{
    zigbee_lamp_t *lamp = require_lamp(lamp_id);
    if (!lamp) {
        return;
    }

    // Zigbee Level Control (cluster 0x0008) uses 0–254.
    // Map 0–255 -> 0–254 by clamping 255 -> 254.
    uint8_t zb_level = (level == 255) ? 254 : level;

    ESP_LOGI(TAG,
             "zigbee_light_set_brightness(lamp_id=%u, addr=0x%04X, ep=%u, app_level=%u, zb_level=%u)",
             lamp->lamp_id,
             lamp->short_addr,
             lamp->endpoint,
             level,
             zb_level);

    esp_err_t err = zigbee_send_level(lamp, zb_level);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send BRIGHTNESS to lamp_id=%u, err=%d", lamp->lamp_id, err);
    }
}

//Not working, might not be this logic, might be the lamp not supporting color control cluster or something else
void zigbee_light_set_color_rgb(uint8_t lamp_id, uint8_t r, uint8_t g, uint8_t b)
{
    zigbee_lamp_t *lamp = require_lamp(lamp_id);
    if (!lamp) {
        return;
    }

    ESP_LOGI(TAG,
             "zigbee_light_set_color_rgb(lamp_id=%u, addr=0x%04X, ep=%u, R=%u, G=%u, B=%u)",
             lamp->lamp_id,
             lamp->short_addr,
             lamp->endpoint,
             r, g, b);

    uint8_t hue = 0;
    uint8_t saturation = 0;
    rgb_to_hue_saturation(r, g, b, &hue, &saturation);

    ESP_LOGI(TAG,
             "Converted RGB->HS for lamp_id=%u: H=%u S=%u",
             lamp->lamp_id,
             hue,
             saturation);

    esp_err_t err = zigbee_send_color_hs(lamp, hue, saturation);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send COLOR to lamp_id=%u, err=%d", lamp->lamp_id, err);
    }
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    if (!signal_struct || !signal_struct->p_app_signal) {
        ESP_LOGW(TAG, "Received invalid Zigbee app signal");
        return;
    }

    esp_zb_app_signal_type_t signal_type = (esp_zb_app_signal_type_t)(*signal_struct->p_app_signal);

    if (signal_type == ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP) {
        ESP_LOGI(TAG, "Signal %s status=%d", esp_zb_zdo_signal_to_string(signal_type), signal_struct->esp_err_status);
        if (signal_struct->esp_err_status == ESP_OK) {
            g_open_join_requested = true;
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Factory-new: start network formation");
            } else {
                ESP_LOGI(TAG, "Not factory-new: force network formation first");
            }

            esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed starting commissioning: %d", err);
            }
        }
        return;
    }

    if (signal_type == ESP_ZB_BDB_SIGNAL_FORMATION) {
        ESP_LOGI(TAG, "Signal %s status=%d", esp_zb_zdo_signal_to_string(signal_type), signal_struct->esp_err_status);
        if (signal_struct->esp_err_status == ESP_OK) {
            ESP_LOGI(TAG, "Network formation success, starting steering");
            esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start steering after formation, err=%d", err);
            }
        } else {
            ESP_LOGW(TAG, "Network formation failed, retrying in 3s");
            esp_zb_scheduler_alarm(zigbee_retry_formation, 0, 3000);
        }
        return;
    }

    if (signal_type == ESP_ZB_BDB_SIGNAL_STEERING) {
        ESP_LOGI(TAG, "Signal %s status=%d", esp_zb_zdo_signal_to_string(signal_type), signal_struct->esp_err_status);
        if (signal_struct->esp_err_status == ESP_OK) {
            g_network_ready = true;
            ESP_LOGI(TAG, "Zigbee network steering success, ready for ON/OFF test");
            if (g_open_join_requested) {
                zigbee_enter_search_mode();
                g_open_join_requested = false;
            }
        } else {
            g_network_ready = false;
            ESP_LOGW(TAG, "Network steering failed, retrying in 3s");
            esp_zb_scheduler_alarm(zigbee_retry_steering, 0, 3000);
        }
        return;
    }

    if (signal_type == ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE) {
        const esp_zb_zdo_signal_device_annce_params_t *join =
            (const esp_zb_zdo_signal_device_annce_params_t *)esp_zb_app_signal_get_params(signal_struct->p_app_signal);

        if (join && signal_struct->esp_err_status == ESP_OK) {
            zigbee_device_join_cb(join, signal_struct);
        } else {
            ESP_LOGW(TAG, "DEVICE_ANNCE received with error status=%d", signal_struct->esp_err_status);
        }
    }
}

static esp_err_t zigbee_send_onoff(zigbee_lamp_t *lamp, uint8_t command_id)
{
    if (!g_zigbee_started || !esp_zb_is_started()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!g_network_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }

    esp_zb_zcl_on_off_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = lamp->short_addr },
            .dst_endpoint = lamp->endpoint,
            .src_endpoint = ZIGBEE_SWITCH_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = command_id,
    };

    uint8_t tsn = esp_zb_zcl_on_off_cmd_req(&cmd);
    esp_zb_lock_release();

    ESP_LOGI(TAG,
             "Sent OnOff cmd=0x%02X lamp_id=%u addr=0x%04X ep=%u tsn=%u",
             command_id,
             lamp->lamp_id,
             lamp->short_addr,
             lamp->endpoint,
             tsn);
    return ESP_OK;
}

static esp_err_t zigbee_send_level(zigbee_lamp_t *lamp, uint8_t level)
{
    if (!g_zigbee_started || !esp_zb_is_started()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!g_network_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }

    esp_zb_zcl_move_to_level_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = lamp->short_addr },
            .dst_endpoint = lamp->endpoint,
            .src_endpoint = ZIGBEE_SWITCH_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .level = level,
        .transition_time = 10,
    };

    uint8_t tsn = esp_zb_zcl_level_move_to_level_with_onoff_cmd_req(&cmd);
    esp_zb_lock_release();

    ESP_LOGI(TAG,
             "Sent Level cmd lamp_id=%u addr=0x%04X ep=%u level=%u tsn=%u",
             lamp->lamp_id,
             lamp->short_addr,
             lamp->endpoint,
             level,
             tsn);
    return ESP_OK;
}

static esp_err_t zigbee_send_color_hs(zigbee_lamp_t *lamp, uint8_t hue, uint8_t saturation)
{
    if (!g_zigbee_started || !esp_zb_is_started()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!g_network_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }

    esp_zb_color_move_to_hue_saturation_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u = { .addr_short = lamp->short_addr },
            .dst_endpoint = lamp->endpoint,
            .src_endpoint = ZIGBEE_SWITCH_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .hue = hue,
        .saturation = saturation,
        .transition_time = 10,
    };

    uint8_t tsn = esp_zb_zcl_color_move_to_hue_and_saturation_cmd_req(&cmd);
    esp_zb_lock_release();

    ESP_LOGI(TAG,
             "Sent Color HS cmd lamp_id=%u addr=0x%04X ep=%u H=%u S=%u tsn=%u",
             lamp->lamp_id,
             lamp->short_addr,
             lamp->endpoint,
             hue,
             saturation,
             tsn);
    return ESP_OK;
}

static void rgb_to_hue_saturation(uint8_t r, uint8_t g, uint8_t b, uint8_t *hue, uint8_t *saturation)
{
    uint8_t max = r;
    if (g > max) {
        max = g;
    }
    if (b > max) {
        max = b;
    }

    uint8_t min = r;
    if (g < min) {
        min = g;
    }
    if (b < min) {
        min = b;
    }

    uint8_t delta = (uint8_t)(max - min);

    if (max == 0) {
        *saturation = 0;
        *hue = 0;
        return;
    }

    *saturation = (uint8_t)(((uint16_t)delta * 254U) / max);

    if (delta == 0) {
        *hue = 0;
        return;
    }

    int16_t h;
    if (max == r) {
        h = (int16_t)(43 * ((int16_t)g - (int16_t)b) / delta);
    } else if (max == g) {
        h = (int16_t)(85 + 43 * ((int16_t)b - (int16_t)r) / delta);
    } else {
        h = (int16_t)(171 + 43 * ((int16_t)r - (int16_t)g) / delta);
    }

    if (h < 0) {
        h += 256;
    }
    if (h > 255) {
        h -= 256;
    }

    *hue = (uint8_t)h;
}

static void zigbee_stack_task(void *arg)
{
    (void)arg;

    esp_zb_platform_config_t platform_cfg = {
        .radio_config = {
            .radio_mode = ZB_RADIO_MODE_NATIVE,
        },
        .host_config = {
            .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,
        },
    };

    esp_err_t err = esp_zb_platform_config(&platform_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_platform_config failed: %d", err);
        vTaskDelete(NULL);
        return;
    }

    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg = {
            .zczr_cfg = {
                .max_children = 10,
            },
        },
    };

    esp_zb_init(&zb_nwk_cfg);
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    esp_zb_on_off_switch_cfg_t switch_cfg = ESP_ZB_DEFAULT_ON_OFF_SWITCH_CONFIG();
    esp_zb_ep_list_t *ep_list = esp_zb_on_off_switch_ep_create(ZIGBEE_SWITCH_ENDPOINT, &switch_cfg);
    if (!ep_list) {
        ESP_LOGE(TAG, "Failed to create Zigbee endpoint list");
        vTaskDelete(NULL);
        return;
    }

    err = esp_zb_device_register(ep_list);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_device_register failed: %d", err);
        vTaskDelete(NULL);
        return;
    }

    err = esp_zb_start(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_start failed: %d", err);
        vTaskDelete(NULL);
        return;
    }

    g_zigbee_started = true;
    ESP_LOGI(TAG, "Zigbee stack started");
    esp_zb_stack_main_loop();
}

static void zigbee_device_join_cb(const esp_zb_zdo_signal_device_annce_params_t *join, esp_zb_app_signal_t *signal_struct)
{
    (void)signal_struct;
    uint16_t short_addr = join->device_short_addr;

    if (short_addr == 0x0000 || short_addr == 0xFFFF) {
        ESP_LOGI(TAG, "Ignoring non-end-device announce short_addr=0x%04X", short_addr);
        return;
    }
    if (zigbee_ieee_is_zero(join->ieee_addr)) {
        ESP_LOGI(TAG, "Ignoring announce with empty IEEE address");
        return;
    }

    ESP_LOGI(TAG,
             "Zigbee device joined! short_addr=0x%04X ieee=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             short_addr,
             join->ieee_addr[7], join->ieee_addr[6], join->ieee_addr[5], join->ieee_addr[4],
             join->ieee_addr[3], join->ieee_addr[2], join->ieee_addr[1], join->ieee_addr[0]);

    zigbee_lamp_t *lamp = zigbee_find_lamp_by_ieee(join->ieee_addr);
    if (!lamp) {
        lamp = zigbee_find_first_free_lamp();
    }

    if (!lamp) {
        ESP_LOGW(TAG, "No free lamp slot for joined device short_addr=0x%04X", short_addr);
        return;
    }

    lamp->short_addr = short_addr;
    memcpy(lamp->ieee_addr, join->ieee_addr, sizeof(esp_zb_ieee_addr_t));
    lamp->joined = true;

    ESP_LOGI(TAG, "Assigned short_addr=0x%04X to lamp_id=%u (%s)", short_addr, lamp->lamp_id, lamp->name);

    int joined_count = zigbee_joined_lamp_count();
    if (joined_count >= REQUIRED_LAMPS) {
        esp_err_t close_err = esp_zb_bdb_open_network(0);
        if (close_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to close permit join after first lamp, err=%d", close_err);
        } else {
            ESP_LOGI(TAG, "Required lamp joined (%d/%d). Permit join closed.", joined_count, REQUIRED_LAMPS);
        }
    }
}