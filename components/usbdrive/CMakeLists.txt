# USB drive app: lists the SD card, then hands it to the USB host as a
# mass-storage drive and shows the transfer status.
idf_component_register(
  SRCS "usbdrive.c"
  INCLUDE_DIRS "."
  REQUIRES tty sdcard usbdev
)
