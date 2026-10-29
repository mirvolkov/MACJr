# ESP32-S3 HAL backend: 2-channel ADC microphone, streamed to the USB host
# as a UAC 2.0 device over USB OTG. Component name (derived from this
# directory) is "mic".
idf_component_register(
  SRCS "mic.c" "mic_uac.c"
  INCLUDE_DIRS "."
  REQUIRES esp_adc
  PRIV_REQUIRES espressif__usb_device_uac usbdev
)
