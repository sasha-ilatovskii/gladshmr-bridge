// Raw BLE → Norns CDC bridge (single board)
//
// Data flow:
//   iPad  →[Raw BLE]→  ble_raw.c  →[g_event_rx_queue]→  event_router_task
//                                                              ↓
//                                              mext_slot_dispatch()
//                                                              ↓
//                                          slot queue [0..N-1]
//                                                              ↓
//                                          mext_slot tasks (one per CDC)
//                                                              ↓
//                                          CDC0..N (grid/arc) → norns (OTG)
//
// iPad app configures the slot layout (grid/arc count and sizes) via the
// BLE config characteristic. monome_slots_apply() persists the config and
// triggers USB re-enumeration so norns sees the updated device set.
//
// LED/ring commands from norns are parsed and absorbed; not relayed to iPad.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "tusb.h"
#include "esp_private/usb_phy.h"

#include "event_queue.h"
#include "monome_slots.h"
#include "mext_slot.h"
#include "ble_raw.h"
#include "usb_descriptors.h"

#define TAG "main"
#define APP_CORE_USB 1
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define BOOT_BUTTON_ACTIVE_LEVEL 0

// -----------------------------------------------------------------------
// TinyUSB task
// -----------------------------------------------------------------------
static void usb_device_task(void *arg) {
    (void)arg;
    while (1) {
        tud_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// -----------------------------------------------------------------------
// Event router — dispatches raw BLE input events to the correct slot
// -----------------------------------------------------------------------
static void event_router_task(void *arg) {
    (void)arg;
    uart_msg_t msg;
    while (1) {
        if (xQueueReceive(g_event_rx_queue, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
            mext_slot_dispatch(&msg);
        }
    }
}

// -----------------------------------------------------------------------
// BOOT button task — toggles incoming BLE packet debug logging
// -----------------------------------------------------------------------
static void boot_button_task(void *arg) {
    (void)arg;

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    int last_level = gpio_get_level(BOOT_BUTTON_GPIO);
    TickType_t last_toggle_tick = 0;

    while (1) {
        int level = gpio_get_level(BOOT_BUTTON_GPIO);
        TickType_t now = xTaskGetTickCount();

        if (last_level != BOOT_BUTTON_ACTIVE_LEVEL &&
            level == BOOT_BUTTON_ACTIVE_LEVEL &&
            now - last_toggle_tick > pdMS_TO_TICKS(250)) {
            last_toggle_tick = now;
            ble_raw_toggle_debug();
        }

        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// -----------------------------------------------------------------------
// USB OTG hardware init
// usb_new_phy() enables the OTG clock AND releases GPIO 19/20 from the
// USB SERIAL JTAG peripheral (which holds the pads at reset). Without
// this, JTAG keeps driving the pads and norns never sees our device.
// -----------------------------------------------------------------------
static usb_phy_handle_t s_phy_handle;

static void usb_otg_hw_init(void) {
    const usb_phy_config_t phy_conf = {
        .controller  = USB_PHY_CTRL_OTG,
        .target      = USB_PHY_TARGET_INT,
        .otg_mode    = USB_OTG_MODE_DEVICE,
        .otg_speed   = USB_PHY_SPEED_FULL,
        .ext_io_conf = NULL,
        .otg_io_conf = NULL,
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy_conf, &s_phy_handle));
}

// -----------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "raw BLE bridge starting");

    // 1. Internal event bus (g_event_rx_queue)
    event_queue_init();

    // 2. Raw BLE peripheral — nvs_flash_init() is called inside
    ble_raw_init();

    // 3. Slot config — load from NVS (defaults: grid16x8 + arc4)
    monome_slots_init();

    // 4. Build USB configuration descriptor for the loaded slot count
    usb_desc_build(g_slot_count);

    // 5. USB OTG PHY
    usb_otg_hw_init();

    // 6. TinyUSB — descriptor callbacks now return the RAM buffer
    if (!tusb_init()) {
        ESP_LOGE(TAG, "TinyUSB init failed");
        return;
    }

    // 7. Mext slot tasks (one per MAX_MONOME_SLOTS, disabled slots sleep)
    mext_slot_init();

    // 8. USB device task + event router
    xTaskCreatePinnedToCore(usb_device_task,   "usb_dev",      4096, NULL, 6,
                            NULL, APP_CORE_USB);
    xTaskCreatePinnedToCore(event_router_task, "event_router", 4096, NULL, 5,
                            NULL, APP_CORE_USB);
    xTaskCreatePinnedToCore(boot_button_task,  "boot_button",  2048, NULL, 4,
                            NULL, APP_CORE_USB);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
