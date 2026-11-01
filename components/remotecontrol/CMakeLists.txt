# Remote control program: last IR command received, sent again on А↑.
# Built into the firmware while it is developed (boot menu built-in);
# meant to move to apps/ later.
idf_component_register(
  SRCS "remotecontrol.c"
  INCLUDE_DIRS "."
  REQUIRES exec ir keyboard monitor display vfs
)
