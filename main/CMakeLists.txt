idf_component_register(SRCS "main.c" "ide_screen.c"
                       INCLUDE_DIRS "."
                       REQUIRES display sdcard esp_timer spi_flash matrix keyboard boot monitor mk52 usbdrive port speaker db radio)
