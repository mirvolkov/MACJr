# System monitor overlay: key MONITOR_KEY toggles a live screen with CPU
# load, RAM, chip temperature and SD card status over the current tty.
idf_component_register(
  SRCS "monitor.c"
  INCLUDE_DIRS "."
  REQUIRES tty keyboard sdcard esp_driver_tsens esp_timer heap esp_psram
)
