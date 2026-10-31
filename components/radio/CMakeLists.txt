# SoftRock SDR app: starts the Si5351 I/Q local oscillator and the USB
# audio stream, and tunes the LO from frequencies typed on the keyboard
# (the active one is saved in config.db).
idf_component_register(
  SRCS "radio.c"
  INCLUDE_DIRS "."
  REQUIRES display keyboard monitor sdr mic db
)
