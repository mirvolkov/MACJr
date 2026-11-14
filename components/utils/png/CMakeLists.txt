# Streaming image decoders: our own PNG chunk / filter / color code on top of
# the zlib inflater (tinfl) in the ESP32-S3 ROM, a baseline JPEG decoder on top
# of the ROM's TJpgDec, and a simple PNG writer. Component name "png".
idf_component_register(
  SRCS "png.c" "jpeg.c" "png_write.c"
  INCLUDE_DIRS "."
  REQUIRES esp_rom
)
