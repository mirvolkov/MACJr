# Streaming PNG decoder: our own chunk / filter / color code on top of the
# zlib inflater (tinfl) that is in the ESP32-S3 ROM. Component name "png".
idf_component_register(
  SRCS "png.c"
  INCLUDE_DIRS "."
  REQUIRES esp_rom
)
