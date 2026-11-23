# The system log: the device /dev/log (logdev.h), a ring of records in
# PSRAM, saved to the SD card as a text file per program in LOG/.
# Component name (derived from this directory) is "logdev": IDF's own
# "log" (esp_log.h) takes the shorter one.
idf_component_register(
  SRCS "logdev.c" "log_file.c"
  INCLUDE_DIRS "."
  REQUIRES sdcard freertos vfs heap devfs
)
