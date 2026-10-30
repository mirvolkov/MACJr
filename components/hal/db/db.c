/*
 * Key-value store in a text file (see db.h). Nothing is cached: every call
 * scans the file a character at a time, so memory use doesn't depend on
 * the file size.
 *
 * Read and create only scan (and append). Update and delete copy the file
 * into DB_TMP with the one line changed, then swap it in: unlink DB_PATH,
 * rename DB_TMP. If power fails between the two, db_init() finds only
 * DB_TMP and renames it back.
 */
#include "db.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DB_TMP SDCARD_MOUNT_POINT "/config.tmp"
#define COPY_CHUNK 256

static const char *TAG = "db";

static SemaphoreHandle_t s_lock;

/* Where a key's line is: [start, text_end) is "key=value", [text_end, end)
 * its line break (none on a last line without one). */
typedef struct {
  long start, text_end, end;
} db_line_t;

static bool exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static bool valid_key(const char *key) {
  return key && *key && !strpbrk(key, "=\r\n");
}

static bool valid_value(const char *value) {
  return value && !strpbrk(value, "\r\n");
}

/* Scan `f` from the start for the first line whose key is `key`. On a hit
 * fill `line` and, if `value` is given, copy the value into it (`len`
 * bytes, terminated; *cut set if it didn't fit). */
static bool find(FILE *f, const char *key, db_line_t *line, char *value, size_t len, bool *cut) {
  size_t key_len = strlen(key);
  rewind(f);
  long pos = 0;
  for (;;) {
    long start = pos;
    size_t k = 0;            /* key characters matched so far */
    bool in_value = false;
    bool match = true;       /* still matching this line's key */
    size_t n = 0;            /* value characters stored */
    int c;
    while ((c = getc(f)) != EOF && c != '\n' && c != '\r') {
      pos++;
      if (in_value) {
        if (match && value && n + 1 < len) {
          value[n++] = (char)c;
        } else if (match && cut) {
          *cut = true;
        }
      } else if (c == '=') {
        in_value = true;
        match = match && k == key_len;
      } else {
        match = match && k < key_len && (unsigned char)key[k] == c;
        k++;
      }
    }
    long text_end = pos;
    if (c == '\r') {
      pos++;
      int c2 = getc(f);
      if (c2 == '\n') {
        pos++;
      } else if (c2 != EOF) {
        ungetc(c2, f);
      }
    } else if (c == '\n') {
      pos++;
    }
    if (in_value && match) {
      if (value) {
        value[n] = '\0';
      }
      *line = (db_line_t){start, text_end, pos};
      return true;
    }
    if (c == EOF) {
      return false;
    }
  }
}

/* Copy bytes [from, to) of `src` to `dst`; to < 0 means up to EOF. */
static bool copy_range(FILE *src, FILE *dst, long from, long to) {
  char buf[COPY_CHUNK];
  if (fseek(src, from, SEEK_SET) != 0) {
    return false;
  }
  for (long left = to < 0 ? -1 : to - from; left != 0;) {
    size_t want = left < 0 || left > (long)sizeof(buf) ? sizeof(buf) : (size_t)left;
    size_t got = fread(buf, 1, want, src);
    if (got == 0) {
      return to < 0 && !ferror(src);
    }
    if (fwrite(buf, 1, got, dst) != got) {
      return false;
    }
    if (left > 0) {
      left -= (long)got;
    }
  }
  return true;
}

/* Rewrite the file with `line` replaced by key=value, or dropped if `key`
 * is NULL; the line break of a replaced line is kept. */
static esp_err_t rewrite(FILE *src, const db_line_t *line, const char *key, const char *value) {
  FILE *dst = fopen(DB_TMP, "wb");
  if (!dst) {
    return ESP_FAIL;
  }
  bool ok = copy_range(src, dst, 0, line->start);
  if (ok && key) {
    ok = fprintf(dst, "%s=%s", key, value) >= 0 && copy_range(src, dst, line->text_end, line->end);
  }
  ok = ok && copy_range(src, dst, line->end, -1);
  ok = fflush(dst) == 0 && ok;
  ok = fsync(fileno(dst)) == 0 && ok;
  ok = fclose(dst) == 0 && ok;
  fclose(src);
  if (!ok) {
    unlink(DB_TMP);
    return ESP_FAIL;
  }
  /* FAT can't rename over an existing file */
  if (unlink(DB_PATH) != 0 || rename(DB_TMP, DB_PATH) != 0) {
    ESP_LOGE(TAG, "swap %s -> %s failed", DB_TMP, DB_PATH);
    return ESP_FAIL;
  }
  return ESP_OK;
}

static bool lock(void) {
  if (!s_lock) {
    return false;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  return true;
}

static void unlock(void) {
  xSemaphoreGive(s_lock);
}

esp_err_t db_init(void) {
  if (!s_lock) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
      return ESP_ERR_NO_MEM;
    }
  }
  lock();
  if (exists(DB_TMP)) {
    if (exists(DB_PATH)) {
      unlink(DB_TMP); /* an unfinished rewrite: the original is intact */
    } else if (rename(DB_TMP, DB_PATH) == 0) {
      ESP_LOGW(TAG, "%s restored from %s", DB_PATH, DB_TMP);
    }
  }
  esp_err_t ret = ESP_OK;
  if (!exists(DB_PATH)) {
    FILE *f = fopen(DB_PATH, "wb");
    if (f) {
      fclose(f);
      ESP_LOGI(TAG, "%s created", DB_PATH);
    } else {
      ESP_LOGE(TAG, "cannot create %s", DB_PATH);
      ret = ESP_FAIL;
    }
  }
  unlock();
  return ret;
}

esp_err_t db_create(const char *key, const char *value) {
  if (!valid_key(key) || !valid_value(value)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!lock()) {
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t ret = ESP_FAIL;
  FILE *f = fopen(DB_PATH, "a+b");
  if (f) {
    db_line_t line;
    if (find(f, key, &line, NULL, 0, NULL)) {
      ret = ESP_ERR_NOT_ALLOWED;
    } else {
      /* start on a new line if the last one has no line break */
      bool need_nl = false;
      if (fseek(f, -1, SEEK_END) == 0) {
        int last = getc(f);
        need_nl = last != EOF && last != '\n' && last != '\r';
      }
      fseek(f, 0, SEEK_END);
      bool ok = (!need_nl || fputc('\n', f) != EOF) && fprintf(f, "%s=%s\n", key, value) >= 0;
      ok = fflush(f) == 0 && ok;
      ok = fsync(fileno(f)) == 0 && ok;
      ret = ok ? ESP_OK : ESP_FAIL;
    }
    fclose(f);
  }
  unlock();
  return ret;
}

esp_err_t db_read(const char *key, char *value, size_t len) {
  if (!valid_key(key) || !value || len == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!lock()) {
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t ret = ESP_FAIL;
  FILE *f = fopen(DB_PATH, "rb");
  if (f) {
    db_line_t line;
    bool cut = false;
    if (find(f, key, &line, value, len, &cut)) {
      ret = cut ? ESP_ERR_INVALID_SIZE : ESP_OK;
    } else {
      ret = ferror(f) ? ESP_FAIL : ESP_ERR_NOT_FOUND;
    }
    fclose(f);
  }
  unlock();
  return ret;
}

/* Shared by update and delete: find `key`, then rewrite without or with
 * the new value. */
static esp_err_t change(const char *key, const char *value) {
  if (!lock()) {
    return ESP_ERR_INVALID_STATE;
  }
  esp_err_t ret = ESP_FAIL;
  FILE *f = fopen(DB_PATH, "rb");
  if (f) {
    db_line_t line;
    if (find(f, key, &line, NULL, 0, NULL)) {
      ret = rewrite(f, &line, value ? key : NULL, value); /* closes f */
      f = NULL;
    } else {
      ret = ferror(f) ? ESP_FAIL : ESP_ERR_NOT_FOUND;
    }
    if (f) {
      fclose(f);
    }
  }
  unlock();
  return ret;
}

esp_err_t db_update(const char *key, const char *value) {
  if (!valid_key(key) || !valid_value(value)) {
    return ESP_ERR_INVALID_ARG;
  }
  return change(key, value);
}

esp_err_t db_delete(const char *key) {
  if (!valid_key(key)) {
    return ESP_ERR_INVALID_ARG;
  }
  return change(key, NULL);
}
