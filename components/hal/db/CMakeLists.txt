# ESP32-S3 HAL backend: key-value config store, a key=value text file on
# the SD card. Component name (derived from this directory) is "db".
idf_component_register(
  SRCS "db.c"
  INCLUDE_DIRS "."
  REQUIRES sdcard freertos
)
