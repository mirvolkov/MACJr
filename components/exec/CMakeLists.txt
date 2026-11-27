# Program loader: runs ELF programs from flash (appfs.c) or the SD card (espressif/elf_loader)
# and exports the sys_* API of sdk/include/macjr.h and the firmware
# functions in exports.c to them.
idf_component_register(
  SRCS "exec.c" "exports.c" "ttydev.c" "appfs.c" "exec_hw.c"
  INCLUDE_DIRS "." "../../sdk/include"
  REQUIRES display keyboard monitor sdcard esp_timer spi_flash db logdev mic sdr usbdev png ir net speaker pthread vfs devfs fatfs wear_levelling
           esp_driver_gpio esp_driver_ledc esp_adc # for upython's machine module (exports.c)
  PRIV_REQUIRES espressif__elf_loader
)
