/*
 * Key-value store (see db.h) on FlashDB's KVDB in file mode: FlashDB
 * treats DB_SECTORS files of DB_SECTOR_SIZE bytes as flash sectors and
 * keeps a log of records in them, with garbage collection and a CRC per
 * record, so a half-written record is skipped after a power cut.
 *
 * FlashDB writes through cached FILE handles and only fflush()es them;
 * FAT commits data on fsync() or fclose(). So every call ends with
 * sync_files(), which fsyncs the cached handles.
 *
 * One recursive mutex covers each call, including FlashDB's own work, so
 * FlashDB gets no lock functions of its own.
 *
 * A successful change is announced to the watches (db_watch.c) after the
 * call has synced and let go of the mutex.
 */
#include "db_priv.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "flashdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DB_OLD_TEXT DB_DIR "/config.db" /* the text store before FlashDB */
#define DB_OLD_DONE DB_DIR "/config.old"
#define IMPORT_LINE 256

_Static_assert(sizeof(DB_NAME) - 1 <= 8, "FlashDB cuts database names to 8 characters");
_Static_assert(DB_KEY_MAX < FDB_KV_NAME_MAX, "keys must fit FlashDB's name buffer");

static const char *TAG = "db";

static SemaphoreHandle_t s_lock;
static struct fdb_kvdb s_kvdb;
static bool s_ready;

static bool valid_key(const char *key) {
  return key && *key && strlen(key) <= DB_KEY_MAX;
}

static esp_err_t to_esp(fdb_err_t err) {
  switch (err) {
  case FDB_NO_ERR:
    return ESP_OK;
  case FDB_KV_NAME_ERR:
    return ESP_ERR_INVALID_ARG;
  case FDB_SAVED_FULL:
    return ESP_ERR_INVALID_SIZE;
  default:
    return ESP_FAIL;
  }
}

/* Commit what FlashDB wrote through its cached files to the card. */
static bool sync_files(void) {
  bool ok = true;
  for (int i = 0; i < FDB_FILE_CACHE_TABLE_SIZE; i++) {
    FILE *f = s_kvdb.parent.cur_file[i];
    if (f) {
      ok = fflush(f) == 0 && ok;
      ok = fsync(fileno(f)) == 0 && ok;
    }
  }
  return ok;
}

static bool lock(void) {
  if (!s_lock) {
    return false;
  }
  xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
  if (!s_ready) {
    xSemaphoreGiveRecursive(s_lock);
    return false;
  }
  return true;
}

/* End of a call: sync, release; ESP_FAIL if `err` was fine but the sync
 * wasn't. */
static esp_err_t unlock(esp_err_t err) {
  bool synced = sync_files();
  xSemaphoreGiveRecursive(s_lock);
  return err == ESP_OK && !synced ? ESP_FAIL : err;
}

/* Announce a change of `key` if `err` says it happened. */
static esp_err_t changed(const char *key, esp_err_t err) {
  if (err == ESP_OK) {
    db_watch_notify(key);
  }
  return err;
}

static bool exists(const char *key) {
  struct fdb_kv kv;
  return fdb_kv_get_obj(&s_kvdb, key, &kv) != NULL;
}

static esp_err_t set(const char *key, const char *value) {
  struct fdb_blob blob;
  return to_esp(fdb_kv_set_blob(&s_kvdb, key, fdb_blob_make(&blob, value, strlen(value))));
}

/* Bring the pairs of the old text store over: "key=value" lines, the
 * first line of a key counts, other lines are skipped. Keys the database
 * already has are kept. */
static void import_old_text(void) {
  FILE *f = fopen(DB_OLD_TEXT, "r");
  if (!f) {
    return;
  }
  char line[IMPORT_LINE];
  int count = 0;
  while (fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = '\0';
    char *eq = strchr(line, '=');
    if (!eq) {
      continue;
    }
    *eq = '\0';
    if (valid_key(line) && !exists(line) && set(line, eq + 1) == ESP_OK) {
      count++;
    }
  }
  fclose(f);
  sync_files();
  if (rename(DB_OLD_TEXT, DB_OLD_DONE) != 0) {
    ESP_LOGW(TAG, "imported %s but cannot rename it", DB_OLD_TEXT);
    return;
  }
  ESP_LOGI(TAG, "imported %d keys from %s (now %s)", count, DB_OLD_TEXT, DB_OLD_DONE);
}

esp_err_t db_init(void) {
  if (!s_lock) {
    s_lock = xSemaphoreCreateRecursiveMutex();
    if (!s_lock) {
      return ESP_ERR_NO_MEM;
    }
  }
  xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
  if (s_ready) {
    xSemaphoreGiveRecursive(s_lock);
    return ESP_OK;
  }

  struct stat st;
  if (stat(DB_DIR, &st) != 0 && mkdir(DB_DIR, 0775) != 0) {
    ESP_LOGE(TAG, "cannot create %s", DB_DIR);
    xSemaphoreGiveRecursive(s_lock);
    return ESP_FAIL;
  }

  bool file_mode = true;
  uint32_t sec_size = DB_SECTOR_SIZE;
  uint32_t max_size = DB_SECTOR_SIZE * DB_SECTORS;
  memset(&s_kvdb, 0, sizeof(s_kvdb));
  fdb_kvdb_control(&s_kvdb, FDB_KVDB_CTRL_SET_FILE_MODE, &file_mode);
  fdb_kvdb_control(&s_kvdb, FDB_KVDB_CTRL_SET_SEC_SIZE, &sec_size);
  fdb_kvdb_control(&s_kvdb, FDB_KVDB_CTRL_SET_MAX_SIZE, &max_size);
  fdb_err_t err = fdb_kvdb_init(&s_kvdb, DB_NAME, DB_DIR, NULL, NULL);
  if (err != FDB_NO_ERR) {
    ESP_LOGE(TAG, "FlashDB init in %s failed: %d", DB_DIR, (int)err);
    xSemaphoreGiveRecursive(s_lock);
    return ESP_FAIL;
  }
  if (db_watch_init() != ESP_OK) {
    ESP_LOGW(TAG, "no change watches");
  }
  s_ready = true;
  import_old_text();
  ESP_LOGI(TAG, "%s/%s.fdb.*: %d x %d bytes", DB_DIR, DB_NAME, DB_SECTORS, DB_SECTOR_SIZE);
  return unlock(ESP_OK);
}

void db_close(void) {
  if (!lock()) {
    return;
  }
  sync_files();
  fdb_kvdb_deinit(&s_kvdb);
  s_ready = false;
  xSemaphoreGiveRecursive(s_lock);
  ESP_LOGI(TAG, "closed");
}

esp_err_t db_create(const char *key, const char *value) {
  if (!valid_key(key) || !value) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!lock()) {
    return ESP_ERR_INVALID_STATE;
  }
  return changed(key, unlock(exists(key) ? ESP_ERR_NOT_ALLOWED : set(key, value)));
}

esp_err_t db_read(const char *key, char *value, size_t len) {
  if (!valid_key(key) || !value || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!lock()) {
    return ESP_ERR_INVALID_STATE;
  }
  struct fdb_blob blob;
  size_t got = fdb_kv_get_blob(&s_kvdb, key, fdb_blob_make(&blob, value, len - 1));
  esp_err_t err = ESP_OK;
  if (got == 0 && !exists(key)) {
    err = ESP_ERR_NOT_FOUND;
  } else if (blob.saved.len > len - 1) {
    err = ESP_ERR_INVALID_SIZE; /* cut short */
  }
  value[got] = '\0';
  return unlock(err);
}

esp_err_t db_update(const char *key, const char *value) {
  if (!valid_key(key) || !value) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!lock()) {
    return ESP_ERR_INVALID_STATE;
  }
  return changed(key, unlock(exists(key) ? set(key, value) : ESP_ERR_NOT_FOUND));
}

esp_err_t db_delete(const char *key) {
  if (!valid_key(key)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!lock()) {
    return ESP_ERR_INVALID_STATE;
  }
  return changed(key, unlock(exists(key) ? to_esp(fdb_kv_del(&s_kvdb, key)) : ESP_ERR_NOT_FOUND));
}
