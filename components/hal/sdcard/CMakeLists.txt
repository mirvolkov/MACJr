# ESP32-S3 HAL backend: SD card over SPI, mounted as a FAT filesystem.
# Component name (derived from this directory) is "sdcard".
idf_component_register(
  SRCS "sdcard.c"
  INCLUDE_DIRS "."
  REQUIRES fatfs esp_driver_sdspi sdmmc
)
