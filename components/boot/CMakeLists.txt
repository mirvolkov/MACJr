# Boot shell: a file navigator over the SD card in the style of DOS
# Navigator; starts programs (*.ELF) and built-ins. Text on the tty.
idf_component_register(
  SRCS "boot.c"
  INCLUDE_DIRS "."
  REQUIRES display keyboard exec sdcard monitor net db usbdev
)
