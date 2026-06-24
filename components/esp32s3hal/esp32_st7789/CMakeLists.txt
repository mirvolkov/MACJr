# Concrete macjr_hal_display_t backend: ESP32-S3 + ST7789 over SPI.
idf_component_register(
  SRCS
    "lcd.c"
    "display_hal.c"
  INCLUDE_DIRS "."
  REQUIRES esp32s3hal esp_lcd driver
)
