// Raw BLE peripheral — NimBLE GATT server
//
// Custom GATT service with two characteristics:
//   1. Data char  (Write Without Response) — receives raw binary event packets
//   2. Config char (Write | Write Without Response) — receives slot layout config
//
// Packet parsing is a simple switch on the first byte; no MIDI state machine.
// MTU is negotiated to 247 bytes (set via sdkconfig CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU).
// 2M PHY is requested on connect for higher throughput.
//
// Input events are enqueued to g_event_rx_queue for event_router_task.

#define LOG_LOCAL_LEVEL ESP_LOG_INFO
#include "ble_raw.h"
#include "monome_slots.h"
#include "event_queue.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_att.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define TAG "ble_raw"
#define BLE_APP_CORE 0

// ---------------------------------------------------------------------------
// UUIDs (128-bit, stored little-endian for NimBLE)
//
// Service:      6D6F6E6F-6D65-0000-0000-000000000000
// Data char:    6D6F6E6F-6D65-0000-0000-000000000001
// Config char:  6D6F6E6F-6D65-0000-0000-000000000002
// State char:   6D6F6E6F-6D65-0000-0000-000000000003  (notify, ESP32 → iPad)
// ---------------------------------------------------------------------------
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x65, 0x6D, 0x6F, 0x6E, 0x6F, 0x6D
);

static const ble_uuid128_t s_data_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x65, 0x6D, 0x6F, 0x6E, 0x6F, 0x6D
);

static const ble_uuid128_t s_cfg_uuid = BLE_UUID128_INIT(
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x65, 0x6D, 0x6F, 0x6E, 0x6F, 0x6D
);

static const ble_uuid128_t s_state_uuid = BLE_UUID128_INIT(
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x65, 0x6D, 0x6F, 0x6E, 0x6F, 0x6D
);

static uint16_t s_data_val_handle;
static uint16_t s_cfg_val_handle;
static uint16_t s_state_val_handle;
static bool s_debug_incoming;

// Active connection handle — set on connect, cleared on disconnect.
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_state_notify_enabled;

#define STATE_TX_QUEUE_LEN 48
#define STATE_TX_MSG_MAX_LEN 65
#define STATE_TX_NOTIFY_MAX_LEN 244

typedef struct {
    uint16_t len;
    uint8_t data[STATE_TX_MSG_MAX_LEN];
} state_tx_msg_t;

static QueueHandle_t s_state_tx_queue;
static uint32_t s_state_drop_count;
static uint32_t s_state_fail_count;

static uint16_t state_notify_payload_max(void) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return 20;
    }

    uint16_t mtu = ble_att_mtu(s_conn_handle);
    if (mtu <= 3) {
        return 20;
    }

    uint16_t payload_max = mtu - 3;
    if (payload_max > STATE_TX_NOTIFY_MAX_LEN) {
        payload_max = STATE_TX_NOTIFY_MAX_LEN;
    }
    return payload_max;
}

// Forward declaration (gap_event_cb is referenced by start_advertising)
static int gap_event_cb(struct ble_gap_event *event, void *arg);

static void request_fast_connection(uint16_t conn_handle) {
    // BLE intervals are in 1.25 ms units. Ask the central for 7.5-15 ms so
    // animation bursts drain quickly, while still allowing iOS to choose.
    struct ble_gap_upd_params params = {
        .itvl_min = 6,
        .itvl_max = 12,
        .latency = 0,
        .supervision_timeout = 200,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    int rc = ble_gap_update_params(conn_handle, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "conn param update request failed: rc=%d", rc);
    }
}

// ---------------------------------------------------------------------------
// Raw packet event type constants (must match iPad app)
// ---------------------------------------------------------------------------
#define RAW_KEY_DOWN     0x10
#define RAW_KEY_UP       0x11
#define RAW_TILT         0x30  // sensor(1) x(1) y(1) z(1)
#define RAW_ENC_DELTA    0x20
#define RAW_ENC_SW_DOWN  0x21
#define RAW_ENC_SW_UP    0x22

static const char *raw_packet_name(uint8_t type) {
    switch (type) {
    case RAW_KEY_DOWN:
        return "key_down";
    case RAW_KEY_UP:
        return "key_up";
    case RAW_ENC_DELTA:
        return "enc_delta";
    case RAW_ENC_SW_DOWN:
        return "enc_sw_down";
    case RAW_ENC_SW_UP:
        return "enc_sw_up";
    case RAW_TILT:
        return "tilt";
    default:
        return "unknown";
    }
}

static void log_raw_packet_bytes(const uint8_t *buf, uint16_t len) {
    char hex[3 * 32 + 1];
    uint16_t shown = len;
    if (shown > 32) {
        shown = 32;
    }

    size_t pos = 0;
    for (uint16_t i = 0; i < shown && pos + 3 < sizeof(hex); i++) {
        int written = snprintf(&hex[pos], sizeof(hex) - pos, "%02x%s", buf[i],
                               i + 1 < shown ? " " : "");
        if (written < 0) {
            break;
        }
        pos += (size_t)written;
    }

    ESP_LOGI(TAG, "rx raw %s type=0x%02x len=%u%s bytes=%s",
             raw_packet_name(buf[0]), buf[0], (unsigned)len,
             len > shown ? " truncated" : "", hex);
}

// ---------------------------------------------------------------------------
// Packet parser — no state machine; each write is one complete event
// ---------------------------------------------------------------------------
static void parse_raw_packet(const uint8_t *buf, uint16_t len) {
    if (len < 2) return;

    if (s_debug_incoming) {
        log_raw_packet_bytes(buf, len);
    }

    uart_msg_t msg = {0};

    switch (buf[0]) {
    case RAW_KEY_DOWN:
        if (len < 3) break;
        msg.type    = UART_MSG_KEY_DOWN;
        msg.data[0] = buf[1];  // x
        msg.data[1] = buf[2];  // y
        msg.len     = 2;
        break;

    case RAW_KEY_UP:
        if (len < 3) break;
        msg.type    = UART_MSG_KEY_UP;
        msg.data[0] = buf[1];  // x
        msg.data[1] = buf[2];  // y
        msg.len     = 2;
        break;

    case RAW_ENC_DELTA:
        if (len < 3) break;
        msg.type    = UART_MSG_ENC_DELTA;
        msg.data[0] = buf[1];  // encoder number
        msg.data[1] = buf[2];  // delta (int8 cast to uint8; mext_slot unpacks it)
        msg.len     = 2;
        break;

    case RAW_ENC_SW_DOWN:
        msg.type    = UART_MSG_ENC_SWITCH_DOWN;
        msg.data[0] = buf[1];  // encoder number
        msg.len     = 1;
        break;

    case RAW_ENC_SW_UP:
        msg.type    = UART_MSG_ENC_SWITCH_UP;
        msg.data[0] = buf[1];  // encoder number
        msg.len     = 1;
        break;

    case RAW_TILT:
        if (len < 5) break;
        msg.type    = UART_MSG_TILT;
        msg.data[0] = buf[1];  // sensor
        msg.data[1] = buf[2];  // x
        msg.data[2] = buf[3];  // y
        msg.data[3] = buf[4];  // z
        msg.len     = 4;
        break;

    default:
        ESP_LOGW(TAG, "unknown packet type 0x%02x len=%u", buf[0], (unsigned)len);
        return;
    }

    if (msg.len > 0) {
        if (s_debug_incoming) {
            switch (msg.type) {
            case UART_MSG_KEY_DOWN:
            case UART_MSG_KEY_UP:
                ESP_LOGI(TAG, "decoded %s x=%u y=%u",
                         msg.type == UART_MSG_KEY_DOWN ? "key_down" : "key_up",
                         (unsigned)msg.data[0], (unsigned)msg.data[1]);
                break;
            case UART_MSG_ENC_DELTA:
                ESP_LOGI(TAG, "decoded enc_delta n=%u delta=%d",
                         (unsigned)msg.data[0], (int8_t)msg.data[1]);
                break;
            case UART_MSG_ENC_SWITCH_DOWN:
            case UART_MSG_ENC_SWITCH_UP:
                ESP_LOGI(TAG, "decoded %s n=%u",
                         msg.type == UART_MSG_ENC_SWITCH_DOWN ? "enc_sw_down" : "enc_sw_up",
                         (unsigned)msg.data[0]);
                break;
            case UART_MSG_TILT:
                ESP_LOGI(TAG, "decoded tilt sensor=%u x=%d y=%d z=%d",
                         (unsigned)msg.data[0], (int8_t)msg.data[1],
                         (int8_t)msg.data[2], (int8_t)msg.data[3]);
                break;
            default:
                break;
            }
        }

        if (xQueueSend(g_event_rx_queue, &msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "event queue full; dropped packet type=0x%02x", buf[0]);
        }
    }
}

void ble_raw_toggle_debug(void) {
    s_debug_incoming = !s_debug_incoming;
    esp_log_level_set(TAG, s_debug_incoming ? ESP_LOG_INFO : ESP_LOG_WARN);
    ESP_LOGW(TAG, "incoming BLE raw debug %s", s_debug_incoming ? "ON" : "OFF");
}

// ---------------------------------------------------------------------------
// GATT data characteristic access callback
// ---------------------------------------------------------------------------
static int data_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[250];
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(ctxt->om, 0, len, buf);
        parse_raw_packet(buf, len);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// GATT state characteristic access callback — notify-only, reads return empty
// ---------------------------------------------------------------------------
static int state_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    // No readable value — state is pushed via notifications only
    return 0;
}

// ---------------------------------------------------------------------------
// GATT config characteristic access callback
// Config write format: [count, type p1 p2, type p1 p2, ...]
// Identical to firmware-ble — calls monome_slots_apply() unchanged.
// ---------------------------------------------------------------------------
static int cfg_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[1 + MAX_MONOME_SLOTS * 3];
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(ctxt->om, 0, len, buf);

        if (len < 1) return 0;
        int count = buf[0];
        if (count > MAX_MONOME_SLOTS) {
            ESP_LOGW(TAG, "slot count %d > max %d", count, MAX_MONOME_SLOTS);
            return 0;
        }
        if ((int)len < 1 + count * 3) {
            ESP_LOGW(TAG, "config write too short");
            return 0;
        }

        monome_slot_t slots[MAX_MONOME_SLOTS];
        for (int i = 0; i < count; i++) {
            slots[i].type   = (monome_slot_type_t)buf[1 + i * 3];
            slots[i].param1 = buf[2 + i * 3];
            slots[i].param2 = buf[3 + i * 3];
        }
        ESP_LOGI(TAG, "config write: %d slots", count);
        monome_slots_apply(slots, count);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// GATT service table
// ---------------------------------------------------------------------------
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // Raw event data — iPad writes key/encoder events here
                .uuid       = &s_data_uuid.u,
                .access_cb  = data_access_cb,
                .val_handle = &s_data_val_handle,
                .flags      = BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // Device config — same wire format as firmware-ble
                .uuid       = &s_cfg_uuid.u,
                .access_cb  = cfg_access_cb,
                .val_handle = &s_cfg_val_handle,
                .flags      = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // State notifications — LED/ring state from norns, pushed to iPad
                .uuid       = &s_state_uuid.u,
                .access_cb  = state_access_cb,
                .val_handle = &s_state_val_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }, // terminator
        },
    },
    { 0 }, // terminator
};

// ---------------------------------------------------------------------------
// GAP advertising
// ---------------------------------------------------------------------------
static void start_advertising(void) {
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields  fields     = {0};
    struct ble_hs_adv_fields  rsp_fields = {0};

    // Primary packet: flags + custom service UUID
    fields.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128             = (ble_uuid128_t *)&s_svc_uuid;
    fields.num_uuids128         = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed: %d", rc);
        return;
    }

    // Scan response: device name (read by iOS when it scans)
    const char *name = ble_svc_gap_device_name();
    rsp_fields.name             = (uint8_t *)name;
    rsp_fields.name_len         = (uint8_t)strlen(name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv rsp set fields failed: %d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                            &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "advertising as '%s'", name);
    }
}

// ---------------------------------------------------------------------------
// GAP event handler
// ---------------------------------------------------------------------------
static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_state_notify_enabled = false;
            if (s_state_tx_queue) {
                xQueueReset(s_state_tx_queue);
            }
            ESP_LOGI(TAG, "connected, conn_handle=%d", s_conn_handle);
            // Request 2M PHY on this connection for higher throughput
            ble_gap_set_prefered_le_phy(event->connect.conn_handle,
                                        BLE_GAP_LE_PHY_2M_MASK,
                                        BLE_GAP_LE_PHY_2M_MASK,
                                        BLE_GAP_LE_PHY_CODED_ANY);
            request_fast_connection(event->connect.conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed, status=%d", event->connect.status);
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_state_notify_enabled = false;
        if (s_state_tx_queue) {
            xQueueReset(s_state_tx_queue);
        }
        ESP_LOGI(TAG, "disconnected, reason=%d — restarting advertising",
                 event->disconnect.reason);
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_state_val_handle) {
            s_state_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "state notifications %s",
                     s_state_notify_enabled ? "enabled" : "disabled");
            if (!s_state_notify_enabled && s_state_tx_queue) {
                xQueueReset(s_state_tx_queue);
            }
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negotiated: conn=%d mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        ESP_LOGI(TAG, "PHY update: tx=%d rx=%d",
                 event->phy_updated.tx_phy, event->phy_updated.rx_phy);
        break;

    default:
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// NimBLE host callbacks
// ---------------------------------------------------------------------------
static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }
    start_advertising();
}

static void on_reset(int reason) {
    ESP_LOGE(TAG, "BLE host reset, reason: %d", reason);
}

// ---------------------------------------------------------------------------
// NimBLE host task
// ---------------------------------------------------------------------------
static void ble_host_task(void *param) {
    ESP_LOGI(TAG, "BLE host task running");
    nimble_port_run();   // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

static void state_tx_task(void *param) {
    (void)param;

    state_tx_msg_t msg;
    uint8_t batch[STATE_TX_NOTIFY_MAX_LEN];
    while (1) {
        if (xQueueReceive(s_state_tx_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_state_notify_enabled) {
            continue;
        }

        uint16_t batch_len = msg.len;
        uint16_t payload_max = state_notify_payload_max();
        memcpy(batch, msg.data, msg.len);

        while (batch_len < payload_max &&
               xQueueReceive(s_state_tx_queue, &msg, 0) == pdTRUE) {
            if (msg.len > payload_max - batch_len) {
                xQueueSendToFront(s_state_tx_queue, &msg, 0);
                break;
            }
            memcpy(&batch[batch_len], msg.data, msg.len);
            batch_len += msg.len;
        }

        struct os_mbuf *om = ble_hs_mbuf_from_flat(batch, batch_len);
        if (!om) {
            s_state_fail_count++;
            if ((s_state_fail_count & 0x3f) == 1) {
                ESP_LOGW(TAG, "state notify mbuf alloc failed, count=%" PRIu32,
                         s_state_fail_count);
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        int rc = ble_gatts_notify_custom(s_conn_handle, s_state_val_handle, om);
        if (rc != 0) {
            s_state_fail_count++;
            if ((s_state_fail_count & 0x3f) == 1) {
                ESP_LOGW(TAG, "state notify failed rc=%d count=%" PRIu32, rc,
                         s_state_fail_count);
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

// ---------------------------------------------------------------------------
// Public: send LED/ring state notification to the connected iPad
// ---------------------------------------------------------------------------
void ble_raw_send_state(const uint8_t *data, uint16_t len) {
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_state_notify_enabled ||
        !s_state_tx_queue) {
        return;
    }

    state_tx_msg_t msg = {0};
    if (len > sizeof(msg.data)) {
        len = sizeof(msg.data);
    }
    msg.len = len;
    memcpy(msg.data, data, len);

    if (xQueueSend(s_state_tx_queue, &msg, 0) != pdTRUE) {
        state_tx_msg_t dropped;
        xQueueReceive(s_state_tx_queue, &dropped, 0);
        if (xQueueSend(s_state_tx_queue, &msg, 0) != pdTRUE) {
            return;
        }

        s_state_drop_count++;
        if ((s_state_drop_count & 0x3f) == 1) {
            ESP_LOGW(TAG, "state notify queue full; dropped old frames count=%" PRIu32,
                     s_state_drop_count);
        }
    }
}

// ---------------------------------------------------------------------------
// Public init
// ---------------------------------------------------------------------------
void ble_raw_init(void) {
    // NVS required for NimBLE bond storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nimble_port_init());

    s_state_tx_queue = xQueueCreate(STATE_TX_QUEUE_LEN, sizeof(state_tx_msg_t));
    if (!s_state_tx_queue) {
        ESP_LOGE(TAG, "state tx queue alloc failed");
        return;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_gatt_svcs));

    ESP_ERROR_CHECK(ble_svc_gap_device_name_set("monome-raw"));

    // Request maximum ATT MTU (247 bytes usable payload per packet).
    // Also set via sdkconfig CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247.
    ble_att_set_preferred_mtu(247);

    nimble_port_freertos_init(ble_host_task);
    xTaskCreatePinnedToCore(state_tx_task, "ble_state_tx", 4096, NULL, 4, NULL,
                            BLE_APP_CORE);
}
