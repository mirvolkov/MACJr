# SDR HAL: Si5351 clock generator (on the shared I2C bus) driving a Tayloe
# quadrature detector with I/Q local-oscillator clocks on CLK0/CLK1.
# Component name is "sdr".
idf_component_register(
  SRCS "sdr.c" "si5351.c"
  INCLUDE_DIRS "."
  PRIV_INCLUDE_DIRS "private"
  REQUIRES esp_driver_i2c i2c_bus
)
