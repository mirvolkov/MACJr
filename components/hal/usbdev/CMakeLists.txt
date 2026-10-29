# USB device HAL: owns TinyUSB on the USB OTG port. One firmware carries
# both the UAC microphone (via espressif/usb_device_uac built with
# CONFIG_USB_DEVICE_UAC_AS_PART) and a mass-storage (MSC) SD card drive;
# usbdev_start() picks which one the host sees. Component name is "usbdev".
idf_component_register(
  SRCS "usbdev.c" "usb_descriptors.c" "usb_msc.c"
  INCLUDE_DIRS "."
  REQUIRES sdmmc
  PRIV_REQUIRES espressif__tinyusb espressif__usb_device_uac esp_hw_support
)

# Our tusb_config.h (AUDIO + MSC) replaces the one usb_device_uac would
# inject without AS_PART; its tusb_uac/ headers supply the audio settings.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(uac_dir espressif__usb_device_uac COMPONENT_DIR)
target_include_directories(${tusb_lib} PUBLIC "${COMPONENT_DIR}/tusb" "${uac_dir}/tusb_uac")

# TinyUSB calls these callbacks but is linked after us, so make the linker
# keep the objects that define them.
target_link_libraries(${COMPONENT_LIB} INTERFACE
  "-u tud_descriptor_device_cb"
  "-u tud_descriptor_configuration_cb"
  "-u tud_descriptor_string_cb"
  "-u tud_msc_read10_cb")
