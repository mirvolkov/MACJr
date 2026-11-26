idf_component_register(SRCS "main.c" "ota.c" "netupd.c"
                       INCLUDE_DIRS "."
                       REQUIRES display sdcard spi_flash matrix keyboard boot monitor speaker db logdev exec ir net app_update esp_app_format esp_http_server esp_timer)
