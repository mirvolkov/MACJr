idf_component_register(
  SRCS
    "src/vc.c"
    "src/ldisc_ntty.c"
    "src/tty.c"
    "src/hal_blit.c"
    "src/font_builtin.c"
  INCLUDE_DIRS "include"
  REQUIRES freertos esp32s3hal
)
