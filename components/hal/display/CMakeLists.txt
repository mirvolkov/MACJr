# Concrete macjr_hal_display_t backend: ESP32-S3 + ST7789 over SPI. The
# macjr_hal_display_t interface itself lives in the tty component -- this
# backend just implements it.
# Component name (derived from this directory) is "display" -- referenced
# as such from main/CMakeLists.txt's REQUIRES.
idf_component_register(
  SRCS
    "lcd.c"
    "display_hal.c"
  INCLUDE_DIRS "."
  REQUIRES tty esp_lcd driver
)
