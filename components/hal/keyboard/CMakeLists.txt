# ESP32-S3 HAL backend: MCP23018 16-bit I2C GPIO expander used as the
# keyboard input chip. Component name (derived from this directory) is
# "keyboard".
idf_component_register(
  SRCS "keyboard.c"
  INCLUDE_DIRS "."
  REQUIRES esp_driver_i2c esp_driver_gpio freertos
)
