# Display HAL: the ST7789 backend (lcd.c, display_hal.c) and, under tty/,
# the hardware-independent text console on top of it -- virtual console,
# line discipline, glyph blitter, built-in 8x16 font, the app header, the
# scrollable list window and the macjr_hal_display_t interface the backend
# implements.
# Component name (derived from this directory) is "display".
idf_component_register(
  SRCS
    "lcd.c"
    "display_hal.c"
    "tty/src/vc.c"
    "tty/src/ldisc_ntty.c"
    "tty/src/tty.c"
    "tty/src/header.c"
    "tty/src/list.c"
    "tty/src/hal_blit.c"
    "tty/src/font_builtin.c"
  INCLUDE_DIRS "." "tty/include"
  REQUIRES freertos esp_lcd driver
)
