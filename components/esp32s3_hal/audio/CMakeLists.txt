# ESP32-S3 HAL backend: I2S TX audio output.
# Component name (derived from this directory) is "audio".
idf_component_register(
  SRCS "audio.c"
  INCLUDE_DIRS "."
  REQUIRES esp_driver_i2s esp_driver_gpio freertos
)
