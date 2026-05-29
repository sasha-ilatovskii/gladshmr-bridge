// USB descriptors for BLE bridge — dynamic CDC-ACM composite device.
//
// usb_desc_build(n) builds a configuration descriptor with n CDC-ACM
// interfaces into a RAM buffer. The TinyUSB callbacks return that buffer,
// so the descriptor reflects whatever slot count was set most recently.
//
// Endpoint assignment per slot i:
//   Notify IN:  0x81 + 2*i
//   Data OUT:   0x02 + 2*i
//   Data IN:    0x82 + 2*i
//
// ESP32-S3 hardware limit: 5 usable IN endpoints → max 2 active slots.

#include "usb_descriptors.h"
#include "monome_slots.h"
#include "tusb.h"
#include <string.h>

// -----------------------------------------------------------------------
// Device descriptor — constant
// -----------------------------------------------------------------------
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x1337,
    .idProduct          = 0x0001,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUF,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

// -----------------------------------------------------------------------
// Dynamic configuration descriptor
// -----------------------------------------------------------------------
#define MAX_CONFIG_DESC_LEN  (TUD_CONFIG_DESC_LEN + MAX_MONOME_SLOTS * TUD_CDC_DESC_LEN)

static uint8_t s_config_desc[MAX_CONFIG_DESC_LEN];

void usb_desc_build(int n_slots) {
    if (n_slots < 0)               n_slots = 0;
    if (n_slots > MAX_MONOME_SLOTS) n_slots = MAX_MONOME_SLOTS;

    int total_len = TUD_CONFIG_DESC_LEN + n_slots * TUD_CDC_DESC_LEN;
    int itf_count = n_slots * 2;  // each CDC IAD = 2 interfaces (comm + data)
    int pos       = 0;

    // Configuration header
    {
        uint8_t hdr[] = {
            TUD_CONFIG_DESCRIPTOR(1, itf_count, 0, total_len, 0x80, 100)
        };
        memcpy(s_config_desc + pos, hdr, sizeof(hdr));
        pos += (int)sizeof(hdr);
    }

    // One CDC IAD per slot
    for (int i = 0; i < n_slots; i++) {
        uint8_t itfnum   = (uint8_t)(i * 2);
        uint8_t ep_notif = (uint8_t)(0x81 + i * 2);
        uint8_t ep_out   = (uint8_t)(0x02 + i * 2);
        uint8_t ep_in    = (uint8_t)(0x82 + i * 2);

        uint8_t cdc[] = {
            TUD_CDC_DESCRIPTOR(itfnum, 0, ep_notif, 8, ep_out, ep_in, 64)
        };
        memcpy(s_config_desc + pos, cdc, sizeof(cdc));
        pos += (int)sizeof(cdc);
    }
}

// -----------------------------------------------------------------------
// String descriptors
// -----------------------------------------------------------------------
static const char *s_desc_str[] = {
    [STRID_LANGID]  = (const char[]){0x09, 0x04},
    [STRID_MANUF]   = "monome",
    [STRID_PRODUCT] = "monome",
    [STRID_SERIAL]  = "m1000001",
};

// -----------------------------------------------------------------------
// TinyUSB descriptor callbacks
// -----------------------------------------------------------------------
const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return s_config_desc;
}

static uint16_t s_desc_str_buf[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    if (index >= STRID_COUNT) return NULL;

    const char *str = s_desc_str[index];
    uint8_t chr_count;

    if (index == STRID_LANGID) {
        memcpy(&s_desc_str_buf[1], str, 2);
        chr_count = 1;
    } else {
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) {
            s_desc_str_buf[1 + i] = str[i];
        }
    }

    s_desc_str_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return s_desc_str_buf;
}
