# Boot menu: the built-in TTY demo and the programs in /BIN on the SD
# card. Text on the tty.
idf_component_register(
  SRCS "boot.c"
  INCLUDE_DIRS "."
  REQUIRES display keyboard esp_app_format exec
)
