#ifndef MACJR_DB_H
#define MACJR_DB_H

#include <stddef.h>

#include "esp_err.h"
#include "sdcard.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Key-value config store: a text file on the SD card, one pair per line.
 *
 *   key=value
 *
 * Lines end in LF, CR or CRLF. A key is everything before the first '=',
 * the value everything after it, both taken exactly (no trimming). Lines
 * without '=' (blank lines, "# comments") are ignored and kept as they are.
 * If a key appears twice, the first line counts.
 *
 * Keys: non-empty, no '=', CR or LF. Values: no CR or LF (may be empty).
 * Otherwise ESP_ERR_INVALID_ARG.
 *
 * Every call opens and closes the file, so nothing stays open (the SD card
 * can be handed to USB at any time; calls then fail with ESP_FAIL). Calls
 * are thread-safe. Before db_init() they return ESP_ERR_INVALID_STATE.
 */
#define DB_DIR SDCARD_MOUNT_POINT "/DB"
#define DB_PATH DB_DIR "/config.db"

/* Create DB_DIR and DB_PATH if they don't exist. Needs the SD card
 * mounted. */
esp_err_t db_init(void);

/* Add a new pair. ESP_ERR_NOT_ALLOWED if the key already exists. */
esp_err_t db_create(const char *key, const char *value);

/* Copy the value of `key` into `value` (`len` bytes, always terminated).
 * ESP_ERR_NOT_FOUND if there is no such key, ESP_ERR_INVALID_SIZE if the
 * value didn't fit (it is then cut short). */
esp_err_t db_read(const char *key, char *value, size_t len);

/* Change the value of an existing key. ESP_ERR_NOT_FOUND if there is none. */
esp_err_t db_update(const char *key, const char *value);

/* Remove the pair. ESP_ERR_NOT_FOUND if there is no such key. */
esp_err_t db_delete(const char *key);

#ifdef __cplusplus
}
#endif

#endif
