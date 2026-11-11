# ESP32-S3 Wi-Fi-band SDR capture (ADC-dump engine). Separate from the Si5351
# HF radio in components/hal/sdr. Needs CONFIG_ESP_PHY_ENABLE_CERT_TEST=y for
# the PHY register helpers (librftest).
idf_component_register(
  SRCS "espsdr.c"
  INCLUDE_DIRS "."
  REQUIRES esp_wifi esp_timer esp_hw_support soc heap esp_rom net
  PRIV_REQUIRES esp_phy
)
