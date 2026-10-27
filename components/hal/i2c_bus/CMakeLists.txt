# Shared I2C master bus (SDA=8, SCL=9) used by every chip on that line:
# MCP23018 keyboard expander, Si5351 SDR clock. Component name is "i2c_bus".
idf_component_register(
  SRCS "i2c_bus.c"
  INCLUDE_DIRS "."
  REQUIRES esp_driver_i2c freertos
)
