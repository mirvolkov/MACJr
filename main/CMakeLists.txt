idf_component_register(SRCS "main.c" "ide_screen.c"
                       INCLUDE_DIRS "."
                       REQUIRES tty display sdcard esp_timer spi_flash matrix keyboard mic sdr boot monitor mk52)
