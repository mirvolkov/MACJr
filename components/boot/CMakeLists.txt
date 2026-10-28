# Boot menu shown at every power-up: picks which mode the
# firmware starts in. Draws straight onto the display HAL (no tty needed).
idf_component_register(
  SRCS "boot.c"
  INCLUDE_DIRS "."
  REQUIRES tty keyboard esp_app_format
)
