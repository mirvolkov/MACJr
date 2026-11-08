#ifndef MACJR_SDCARD_H
#define MACJR_SDCARD_H

#include <stdint.h>

#include <stdbool.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

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

/* Give up the filesystem and open the card for raw sector access (e.g. to
 * expose it as a USB drive): unmounts SDCARD_MOUNT_POINT if mounted, then
 * re-initializes the card without FAT. `*out` stays valid until
 * sdcard_close_raw(); use it with sdmmc_read_sectors() /
 * sdmmc_write_sectors(). */
esp_err_t sdcard_open_raw(sdmmc_card_t **out);

/* Back from sdcard_open_raw(): release the raw card and mount the
 * filesystem again. Nobody may be using the raw card any more. Also works
 * after a failed sdcard_open_raw(); does nothing if already mounted. */
esp_err_t sdcard_close_raw(void);

/* Unmount the card and free the SPI bus. */
void sdcard_deinit(void);

/* True if `path` (under SDCARD_MOUNT_POINT) has the FAT hidden or system
 * attribute, like "System Volume Information". False when it can't be
 * read. */
bool sdcard_is_hidden(const char *path);

#ifdef __cplusplus
}
#endif

#endif
