idf_component_register(SRCS "main_esp32.c" "ide_screen.c"
                       INCLUDE_DIRS "."
                       REQUIRES tty display sdcard esp_timer spi_flash matrix)
