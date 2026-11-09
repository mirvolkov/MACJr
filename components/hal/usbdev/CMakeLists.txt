# USB device HAL: owns TinyUSB on the USB OTG port. One firmware carries
# both the UAC microphone (via espressif/usb_device_uac built with
# CONFIG_USB_DEVICE_UAC_AS_PART) and a mass-storage (MSC) SD card drive;
# usbdev_start() picks which one the host sees. usb_kbd_start() turns the
# port into a USB host for a keyboard instead. Component name is "usbdev".
idf_component_register(
  SRCS "usbdev.c" "usb_descriptors.c" "usb_msc.c" "usb_kbd.c"
  INCLUDE_DIRS "."
  REQUIRES sdmmc
  PRIV_REQUIRES espressif__tinyusb espressif__usb_device_uac esp_hw_support keyboard
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

# TinyUSB's host side (usbh, the DWC2 host driver, HID host) is in the
# package but not in its build: compile it into the TinyUSB library, with
# its flags. For the USB keyboard (usb_kbd.c).
idf_component_get_property(tusb_dir espressif__tinyusb COMPONENT_DIR)
target_sources(${tusb_lib} PRIVATE
  "${tusb_dir}/src/host/usbh.c"
  "${tusb_dir}/src/host/hub.c"
  "${tusb_dir}/src/portable/synopsys/dwc2/hcd_dwc2.c"
  "${tusb_dir}/src/class/hid/hid_host.c")
