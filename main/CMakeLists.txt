idf_component_register(SRCS "MACJr.c"
                       INCLUDE_DIRS "."
                       REQUIRES tty esp32_st7789 esp_timer spi_flash)
