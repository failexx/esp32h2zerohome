// This file is part of the ESP32-H2 Zigbee and BLE bridge project.
// Developed using the Espressif IoT Development Framework (ESP-IDF).
// Zigbee functionality is implemented using the ESP-Zigbee library.

//Authour Felix Bengtsson

#include "ble-bridge.h"
#include "commands.h"

#include <string.h>

#include "esp_log.h"
#include "esp_err.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE_BRIDGE";

static uint8_t own_addr_type;
static uint16_t g_cmd_char_val_handle;

// Forward declarations
static int gatt_svr_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt,
                                  void *arg);
static int ble_bridge_gap_event(struct ble_gap_event *event, void *arg);
static void ble_bridge_advertise(void);
static void ble_bridge_host_task(void *param);
static void ble_bridge_on_sync(void);

// GATT table: one service (0xFFF0) with a write-characteristic (0xFFF1)
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFFF0),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0xFFF1),
                .access_cb = gatt_svr_chr_access_cb,
                .val_handle = &g_cmd_char_val_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_READ,
            },
            { 0 } // terminator for characteristics
        },
    },
    { 0 } // terminator for services
};

// Initialize GATT server (register services/characteristics)
static int gatt_svr_init(void)
{
    int rc;

    // Standard GAP/GATT services (device name etc.)
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return rc;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return rc;
    }

    return 0;
}

// Callback when a client reads/writes our characteristic
static int gatt_svr_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt,
                                  void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        // Extract data from om (mbuf) to a regular buffer
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len <= 0) {
            return 0;
        }

        uint8_t buf[32];
        if (len > (int)sizeof(buf)) {
            len = sizeof(buf);
        }

        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_hs_mbuf_to_flat failed: %d", rc);
            return BLE_ATT_ERR_UNLIKELY;
        }

        ESP_LOGI(TAG, "Received BLE HEX (%d bytes):", len);
        for (int i = 0; i < len; i++) {
            ESP_LOGI(TAG, "  0x%02X", buf[i]);
        }

        // Forward to your hex command parser
        commands_handle_hex(buf, (uint16_t)len);

        return 0;
    }

    case BLE_GATT_ACCESS_OP_READ_CHR:
        // Currently, we just return OK without data.
        return 0;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

// GAP event: connect/disconnect etc.
static int ble_bridge_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "Client connected");
        } else {
            ESP_LOGW(TAG, "Connection failed; restarting advertising");
            ble_bridge_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Client disconnected; restarting advertising");
        ble_bridge_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising complete; restarting");
        ble_bridge_advertise();
        return 0;

    default:
        return 0;
    }
}

// Start BLE advertising
static void ble_bridge_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = "FelixBridge";
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;   // Connectable
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_bridge_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Started advertising as \"%s\"", name);
    }
}

// Sync callback: called when the BLE stack is ready
static void ble_bridge_on_sync(void)
{
    int rc;
    uint8_t addr_val[6] = {0};

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_copy_addr failed: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE address: %02X:%02X:%02X:%02X:%02X:%02X",
             addr_val[5], addr_val[4], addr_val[3],
             addr_val[2], addr_val[1], addr_val[0]);

    // Start advertising
    ble_bridge_advertise();
}

// NimBLE host task (runs in its own FreeRTOS task)
static void ble_bridge_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run(); // Blocks until NimBLE stops
    nimble_port_freertos_deinit();
}

// Declared in the NimBLE port (available in ESP-IDF)
void ble_store_config_init(void);

void ble_bridge_init(void)
{
    ESP_LOGI(TAG, "Initializing NimBLE (host only)...");

    // Initialize the NimBLE host
    nimble_port_init();

    // Initialize NimBLE storage (bonding etc.)
    ble_store_config_init();

    int rc = gatt_svr_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt_svr_init failed: %d", rc);
        return;
    }

    // Set callback when the BLE stack is "synced" and ready
    ble_hs_cfg.sync_cb = ble_bridge_on_sync;

    // Start the NimBLE host in its own FreeRTOS task
    nimble_port_freertos_init(ble_bridge_host_task);

    ESP_LOGI(TAG, "BLE bridge init complete");
}

