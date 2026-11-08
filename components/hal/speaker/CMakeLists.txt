# ESP32-S3 HAL backend: I2S TX speaker output.
# Component name (derived from this directory) is "speaker".
idf_component_register(
  SRCS "wav.c" "speaker.c"
  INCLUDE_DIRS "."
  REQUIRES esp_driver_i2s esp_driver_gpio freertos db
)
