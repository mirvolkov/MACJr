# ESP32-S3 HAL backend: networking. For now the Wi-Fi station: scan,
# connect with a password, IP address. Started on first use, not at boot.
# Component name (derived from this directory) is "net".
idf_component_register(
  SRCS "net.c"
  INCLUDE_DIRS "."
  REQUIRES esp_wifi esp_hw_support
  PRIV_REQUIRES esp_netif esp_event nvs_flash freertos
)
