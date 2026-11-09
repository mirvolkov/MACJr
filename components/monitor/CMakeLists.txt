# System monitor overlay: key MONITOR_KEY toggles a live screen with CPU
# load, RAM, chip temperature and SD card status over the current tty.
# Its task also answers SCREENSHOT_KEY (the screen as a PNG on the SD
# card) and USB_KBD_KEY (USB keyboard on / off).
idf_component_register(
  SRCS "monitor.c" "screenshot.c"
  INCLUDE_DIRS "."
  REQUIRES display keyboard sdcard png usbdev esp_driver_tsens esp_timer heap esp_psram
)
