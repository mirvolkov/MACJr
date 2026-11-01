# ESP32-S3 HAL backend: IR remote control receiver (demodulator on GPIO5)
# and decoder, and transmitter (IR LED on GPIO4, RMT). Component name
# (derived from this directory) is "ir".
idf_component_register(
  SRCS "ir_rx.c" "ir_tx.c"
  INCLUDE_DIRS "."
  REQUIRES esp_driver_gpio esp_driver_rmt esp_timer freertos
)
