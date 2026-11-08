idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES display sdcard spi_flash matrix keyboard boot monitor speaker db exec ir net)
