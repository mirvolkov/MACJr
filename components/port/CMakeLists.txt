# Port app: shows the board's port layout picture (a PNG on the SD card)
# full screen.
idf_component_register(
  SRCS "port.c"
  INCLUDE_DIRS "."
  REQUIRES tty png sdcard
)
