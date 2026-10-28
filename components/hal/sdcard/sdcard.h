#ifndef MACJR_SDCARD_H
#define MACJR_SDCARD_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount point used by sdcard_init() once it succeeds. */
#define SDCARD_MOUNT_POINT "/sdcard"

/* Bring up the SPI bus and mount the SD card at SDCARD_MOUNT_POINT (FAT,
 * via esp_vfs_fat) so the rest of the firmware can fopen()/opendir() under
 * it like any other path. Safe to call once at startup. */
esp_err_t sdcard_init(void);

/* Filesystem size and free space of the mounted card. Returns
 * ESP_ERR_INVALID_STATE when no card is mounted. */
esp_err_t sdcard_info(uint64_t *total_bytes, uint64_t *free_bytes);

/* Unmount the card and free the SPI bus. */
void sdcard_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
