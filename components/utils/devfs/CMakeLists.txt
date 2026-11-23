# The /dev directory (devfs.h): lists the devices the firmware registered,
# so that opendir("/dev") and the navigator see them. Component name
# (derived from this directory) is "devfs".
idf_component_register(
  SRCS "devfs.c"
  INCLUDE_DIRS "."
  REQUIRES vfs freertos
)
