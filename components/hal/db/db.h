#ifndef MACJR_DB_H
#define MACJR_DB_H

#include <stddef.h>

#include "esp_err.h"
#include "sdcard.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Key-value store on the SD card: a FlashDB key-value database (KVDB) in
 * DB_DIR, kept in DB_SECTORS sector files config.fdb.0 .. config.fdb.N
 * of DB_SECTOR_SIZE bytes each. Values are strings.
 *
 * Keys: 1 to DB_KEY_MAX characters. Otherwise ESP_ERR_INVALID_ARG. A
 * value must fit in a sector with its key and header, else
 * ESP_ERR_INVALID_SIZE.
 *
 * FlashDB keeps up to two sector files open between calls; every call
 * ends with them flushed and synced, so what a call wrote survives a
 * power cut. Those files must not outlive the filesystem: db_close()
 * before the card is unmounted, db_init() after it is back. Calls are
 * thread-safe. Before db_init() and after db_close() they return
 * ESP_ERR_INVALID_STATE.
 */
#define DB_DIR SDCARD_MOUNT_POINT "/DB"
#define DB_NAME "config" /* file names: at most 8 characters */
#define DB_SECTOR_SIZE 4096
#define DB_SECTORS 4
#define DB_KEY_MAX 63

/* Create DB_DIR and open the database, creating its files if needed, and
 * register DB_WATCH_DEV. A config.db from the old key=value text store is
 * imported once (keys the database doesn't have yet) and renamed to
 * config.old. Needs the SD card mounted. */
esp_err_t db_init(void);

/* Sync and close the database files. Watches stay open. */
void db_close(void);

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

/*
 * Watching for changes. A watch is a file descriptor on DB_WATCH_DEV:
 * after each successful db_create(), db_update() or db_delete() of a key
 * starting with its prefix, read() on it gets a line "<key>\n". read()
 * blocks until there is one (or fails with EAGAIN under O_NONBLOCK), and
 * select() reports the watch readable while lines are waiting, so a
 * program can wait for changes together with keys. If a watch isn't read
 * and its queue fills up, the queued lines are replaced by one "*\n":
 * changes were lost, read the values again. close() ends the watch.
 */
#define DB_WATCH_DEV "/dev/db"
#define DB_MAX_WATCHES 4 /* open at the same time */

/* Watch keys starting with `prefix` ("" for all): opens
 * DB_WATCH_DEV "/<prefix>" read-only. The descriptor, or -1 (errno set). */
int db_watch(const char *prefix);

#ifdef __cplusplus
}
#endif

#endif
