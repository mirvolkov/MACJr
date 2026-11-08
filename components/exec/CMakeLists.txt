# Program loader: runs ELF programs from the SD card (espressif/elf_loader)
# and exports the sys_* API of sdk/include/macjr.h and the firmware
# functions in exports.c to them.
idf_component_register(
  SRCS "exec.c" "exports.c"
  INCLUDE_DIRS "." "../../sdk/include"
  REQUIRES display keyboard monitor sdcard esp_timer spi_flash db mic sdr usbdev png ir net speaker
  PRIV_REQUIRES espressif__elf_loader
)
