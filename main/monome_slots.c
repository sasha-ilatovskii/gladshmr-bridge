// Monome device slot configuration — NVS persistence + USB re-enumeration.

#include "monome_slots.h"
#include "usb_descriptors.h"
#include "nvs.h"
#include "tusb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG   "monome_slots"
#define NVS_NS "m_slots"

// Default: one grid (16×8) + one arc (4 rings)
monome_slot_t g_slots[MAX_MONOME_SLOTS] = {
    { MONOME_SLOT_GRID, 16, 8 },
    { MONOME_SLOT_ARC,   4, 0 },
    { MONOME_SLOT_DISABLED, 0, 0 },
    { MONOME_SLOT_DISABLED, 0, 0 },
};
int g_slot_count = 2;

// -----------------------------------------------------------------------
// NVS
// -----------------------------------------------------------------------
static void load_from_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved config, using defaults");
        return;
    }
    uint8_t count = 0;
    if (nvs_get_u8(h, "count", &count) != ESP_OK || count > MAX_MONOME_SLOTS) {
        nvs_close(h);
        return;
    }
    g_slot_count = count;
    for (int i = 0; i < count; i++) {
        char key[8];
        uint8_t v;
        snprintf(key, sizeof(key), "t%d", i);
        if (nvs_get_u8(h, key, &v) == ESP_OK) g_slots[i].type = (monome_slot_type_t)v;
        snprintf(key, sizeof(key), "p1%d", i);
        if (nvs_get_u8(h, key, &v) == ESP_OK) g_slots[i].param1 = v;
        snprintf(key, sizeof(key), "p2%d", i);
        if (nvs_get_u8(h, key, &v) == ESP_OK) g_slots[i].param2 = v;
    }
    nvs_close(h);
}

static void save_to_nvs(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, config not saved");
        return;
    }
    nvs_set_u8(h, "count", (uint8_t)g_slot_count);
    for (int i = 0; i < g_slot_count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "t%d", i);
        nvs_set_u8(h, key, (uint8_t)g_slots[i].type);
        snprintf(key, sizeof(key), "p1%d", i);
        nvs_set_u8(h, key, g_slots[i].param1);
        snprintf(key, sizeof(key), "p2%d", i);
        nvs_set_u8(h, key, g_slots[i].param2);
    }
    nvs_commit(h);
    nvs_close(h);
}

// -----------------------------------------------------------------------
// USB re-enumeration — runs in a short-lived task so the BLE
// characteristic write callback can return immediately.
// -----------------------------------------------------------------------
static void reenum_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(50));   // let BLE write complete
    ESP_LOGI(TAG, "USB disconnect");
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));  // host detects disconnect
    ESP_LOGI(TAG, "USB reconnect");
    tud_connect();
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------
void monome_slots_init(void) {
    load_from_nvs();
    ESP_LOGI(TAG, "active slots: %d", g_slot_count);
    for (int i = 0; i < g_slot_count; i++) {
        const char *t = g_slots[i].type == MONOME_SLOT_GRID ? "grid"
                      : g_slots[i].type == MONOME_SLOT_ARC  ? "arc"
                      : "disabled";
        ESP_LOGI(TAG, "  [%d] %s p1=%d p2=%d", i, t, g_slots[i].param1, g_slots[i].param2);
    }
}

void monome_slots_apply(const monome_slot_t *slots, int count) {
    if (count < 0) count = 0;
    if (count > MAX_MONOME_SLOTS) count = MAX_MONOME_SLOTS;

    g_slot_count = count;
    for (int i = 0; i < count; i++) {
        g_slots[i] = slots[i];
    }
    for (int i = count; i < MAX_MONOME_SLOTS; i++) {
        g_slots[i].type = MONOME_SLOT_DISABLED;
    }

    save_to_nvs();
    usb_desc_build(count);  // rebuild config descriptor before reconnect

    ESP_LOGI(TAG, "slot config applied (%d slots), scheduling re-enum", count);
    xTaskCreate(reenum_task, "reenum", 2048, NULL, 7, NULL);
}
