# Streaming PNG decoder: our own chunk / filter / color code on top of the
# zlib inflater (tinfl) that is in the ESP32-S3 ROM, and a simple PNG writer.
# Component name "png".
idf_component_register(
  SRCS "png.c" "png_write.c"
  INCLUDE_DIRS "."
  REQUIRES esp_rom
)
