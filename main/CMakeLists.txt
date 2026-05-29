idf_component_register(
    SRCS
        "main.c"
        "ble_raw.c"
        "event_queue.c"
        "monome_slots.c"
        "mext_slot.c"
        "usb_descriptors.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        nvs_flash
        bt
        esp_driver_gpio
        espressif__tinyusb
        esp_hw_support
)

# The managed espressif/tinyusb component doesn't search user directories for
# tusb_config.h — inject our main/ directory into its include path explicitly.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Force the linker to pull usb_descriptors.o out of libmain.a.
# tinyusb calls these callbacks but they're only defined here; without -u the
# linker skips usb_descriptors.o (nothing in main.o references them directly).
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,tud_descriptor_device_cb"
    "-Wl,-u,tud_descriptor_configuration_cb"
    "-Wl,-u,tud_descriptor_string_cb"
)
