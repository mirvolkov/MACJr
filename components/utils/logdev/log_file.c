/*
 * The log on the SD card (see logdev.h): a text file per program in
 * LOG_DIR, <name>.LOG, a line per record, "<id> <level> <tag> <text>";
 * when it would grow past LOG_FILE_MAX it becomes <name>.OLD (the one
 * before goes) and a new <name>.LOG starts.
 *
 * Saving opens a file, appends, syncs and closes it, so no file stays
 * open (the card takes only a few at a time, sdcard.c). FAT updates a
 * file's size when it is synced, so a power cut loses at most what was
 * appended since; a line cut short at the end is skipped when read.
 *
 * At boot only the files' ends are read: the last LAST_BYTES for the
 * newest number, the last TAIL_BYTES for the records that come back into
 * memory (about LOG_RING lines, so one busy program can fill the ring).
 * A file that doesn't end with a newline (a line cut short) gets one
 * before anything is appended, so the cut line can't swallow the next.
 */
#include "logdev_priv.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define EXT_LOG    ".LOG"
#define EXT_OLD    ".OLD"
#define LAST_BYTES 512
#define TAIL_BYTES 24576 /* LOG_RING lines of ~48 bytes */
#define LINE_MAX   (12 + 3 + LOG_NAME_MAX + LOG_BODY_MAX + 2)
#define PATH_MAX_  (sizeof(LOG_DIR) + 1 + LOG_NAME_MAX + 4)

static const char *TAG = "log_file";

static char *s_tail; /* TAIL_BYTES + 1, in PSRAM */

/* A program's file name: its name with what FAT can't take as '_'. */
static void file_name(const char *app, char *name) {
  int n = 0;
  for (const char *p = app; *p && n < LOG_NAME_MAX - 1; p++) {
    char c = *p;
    name[n++] = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-'
                    ? c
                    : '_';
  }
  if (n == 0) {
    name[n++] = '_';
  }
  name[n] = '\0';
}

static void file_path(char *path, const char *name, const char *ext) {
  snprintf(path, PATH_MAX_, LOG_DIR "/%s%s", name, ext);
}

/* ---- writing ------------------------------------------------------------- */

/* "<id> <L> <tag> <text>\n"; the length. */
static int format_line(const log_rec_t *rec, char *line) {
  int n = snprintf(line, LINE_MAX, "%lu %c %s ", (unsigned long)rec->id, log_level_char(rec->level),
                   rec->tag[0] ? rec->tag : "-");
  for (int i = 0; i < rec->len && n < LINE_MAX - 2; i++) {
    unsigned char c = (unsigned char)rec->body[i];
    line[n++] = c < 0x20 || c == 0x7F ? ' ' : (char)c;
  }
  line[n++] = '\n';
  line[n] = '\0';
  return n;
}

/* Sync and close; false if the card didn't take it. */
static bool close_synced(FILE *f) {
  bool ok = fflush(f) == 0;
  ok = fsync(fileno(f)) == 0 && ok;
  return fclose(f) == 0 && ok;
}

/* Open `name`'s .LOG for appending; its size in `size`. */
static FILE *open_log(const char *name, long *size) {
  char path[PATH_MAX_];
  file_path(path, name, EXT_LOG);
  FILE *f = fopen(path, "a+");
  if (!f) {
    ESP_LOGW(TAG, "cannot open %s", path);
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  *size = ftell(f);
  if (*size > 0 && fseek(f, -1, SEEK_END) == 0 && fgetc(f) != '\n') {
    fseek(f, 0, SEEK_END);
    fputc('\n', f); /* end the line a power cut left cut short */
    (*size)++;
  }
  return f;
}

/* <name>.LOG becomes <name>.OLD, the one before goes. */
static void rotate(const char *name) {
  char log_path[PATH_MAX_], old_path[PATH_MAX_];
  file_path(log_path, name, EXT_LOG);
  file_path(old_path, name, EXT_OLD);
  unlink(old_path);
  if (rename(log_path, old_path) != 0) {
    ESP_LOGW(TAG, "cannot rename %s", log_path);
    unlink(log_path); /* start over rather than grow without end */
  }
}

uint32_t log_file_append(const log_rec_t *recs, int n) {
  char line[LINE_MAX], cur[LOG_NAME_MAX] = "";
  FILE *f = NULL;
  long size = 0;
  uint32_t saved = 0;   /* the newest record surely on the card */
  uint32_t written = 0; /* the newest one in the open file */
  for (int i = 0; i < n; i++) {
    const log_rec_t *rec = &recs[i];
    char name[LOG_NAME_MAX];
    file_name(rec->app, name);
    if (strcmp(name, cur) != 0) { /* another program's file */
      if (f) {
        if (!close_synced(f)) {
          return saved;
        }
        saved = written;
      }
      f = open_log(name, &size);
      if (!f) {
        return saved;
      }
      snprintf(cur, sizeof(cur), "%s", name);
    }
    int len = format_line(rec, line);
    if (size > 0 && size + len > LOG_FILE_MAX) {
      if (!close_synced(f)) {
        return saved;
      }
      saved = written;
      rotate(name);
      f = open_log(name, &size);
      if (!f) {
        return saved;
      }
    }
    if (fwrite(line, 1, len, f) != (size_t)len) {
      fclose(f);
      return saved;
    }
    size += len;
    written = rec->id;
  }
  if (f) {
    if (!close_synced(f)) {
      return saved;
    }
    saved = written;
  }
  return saved;
}

/* ---- reading ------------------------------------------------------------- */

/* A line ("<id> <L> <tag> <text>", no newline) as a record of `app`. */
static bool parse_line(char *line, const char *app, log_rec_t *rec) {
  char *p;
  unsigned long id = strtoul(line, &p, 10);
  if (id == 0 || p == line || *p++ != ' ' || !*p) {
    return false;
  }
  const char *lv = strchr(LOG_LEVELS, *p);
  if (!lv || *p == '\0' || p[1] != ' ') {
    return false;
  }
  p += 2;
  char *tag = p;
  while (*p && *p != ' ') {
    p++;
  }
  if (*p != ' ' || p - tag >= LOG_NAME_MAX) {
    return false;
  }
  *p++ = '\0';
  memset(rec, 0, LOG_REC_HEAD);
  rec->id = (uint32_t)id;
  rec->level = (uint8_t)(lv - LOG_LEVELS);
  snprintf(rec->app, LOG_NAME_MAX, "%s", app);
  snprintf(rec->tag, LOG_NAME_MAX, "%s", strcmp(tag, "-") == 0 ? "" : tag);
  size_t len = strnlen(p, LOG_BODY_MAX - 1);
  memcpy(rec->body, p, len);
  rec->body[len] = '\0';
  rec->len = (uint16_t)len;
  return true;
}

typedef void (*line_fn_t)(const log_rec_t *rec, void *ctx);

/* Call `fn` with each whole line among the last `bytes` of `path`. */
static void read_tail(const char *path, const char *app, size_t bytes, line_fn_t fn, void *ctx) {
  static log_rec_t s_rec; /* too big for the stack */
  FILE *f = fopen(path, "r");
  if (!f) {
    return;
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  long start = size > (long)bytes ? size - (long)bytes : 0;
  fseek(f, start, SEEK_SET);
  size_t got = fread(s_tail, 1, bytes, f);
  fclose(f);
  s_tail[got] = '\0';

  char *p = s_tail;
  if (start > 0) { /* the first line is cut: skip it */
    p = strchr(p, '\n');
    p = p ? p + 1 : s_tail + got;
  }
  for (;;) {
    char *nl = strchr(p, '\n');
    if (!nl) {
      break; /* none left, or one cut short by a power cut */
    }
    *nl = '\0';
    if (parse_line(p, app, &s_rec)) {
      fn(&s_rec, ctx);
    }
    p = nl + 1;
  }
}

/* Call `fn(path, program)` for each .LOG and .OLD file in LOG_DIR. */
static void each_file(void (*fn)(const char *path, const char *app, void *ctx), void *ctx) {
  static char names[2 * LOG_MAX_FILES][LOG_NAME_MAX + 4];
  DIR *dir = opendir(LOG_DIR);
  if (!dir) {
    return;
  }
  int count = 0;
  struct dirent *e;
  while ((e = readdir(dir)) != NULL && count < 2 * LOG_MAX_FILES) {
    const char *dot = strrchr(e->d_name, '.');
    size_t len = dot ? (size_t)(dot - e->d_name) : 0;
    size_t full = strlen(e->d_name);
    if (len > 0 && len < LOG_NAME_MAX && full < sizeof(names[0]) &&
        (strcasecmp(dot, EXT_LOG) == 0 || strcasecmp(dot, EXT_OLD) == 0)) {
      memcpy(names[count++], e->d_name, full + 1);
    }
  }
  closedir(dir);
  for (int i = 0; i < count; i++) {
    char path[PATH_MAX_], app[LOG_NAME_MAX];
    snprintf(app, sizeof(app), "%.*s", (int)(strrchr(names[i], '.') - names[i]), names[i]);
    snprintf(path, sizeof(path), LOG_DIR "/%s", names[i]);
    fn(path, app, ctx);
  }
}

static void max_id(const log_rec_t *rec, void *ctx) {
  uint32_t *last = ctx;
  if (rec->id > *last) {
    *last = rec->id;
  }
}

static void last_of(const char *path, const char *app, void *ctx) {
  read_tail(path, app, LAST_BYTES, max_id, ctx);
}

uint32_t log_file_last(void) {
  uint32_t last = 0;
  if (s_tail) {
    each_file(last_of, &last);
  }
  return last;
}

typedef struct {
  uint32_t from;
  log_file_cb_t cb;
  void *ctx;
} load_t;

static void load_rec(const log_rec_t *rec, void *ctx) {
  load_t *l = ctx;
  if (rec->id >= l->from) {
    l->cb(rec, l->ctx);
  }
}

static void load_file(const char *path, const char *app, void *ctx) {
  read_tail(path, app, TAIL_BYTES, load_rec, ctx);
}

void log_file_load(uint32_t from, log_file_cb_t cb, void *ctx) {
  load_t l = {.from = from, .cb = cb, .ctx = ctx};
  if (s_tail) {
    each_file(load_file, &l);
  }
}

/* ---- the directory --------------------------------------------------------- */

/* The FlashDB stores of an earlier version (<name>.fdb.N): gone. */
static void remove_fdb_files(void) {
  static char names[8][32];
  for (;;) {
    DIR *dir = opendir(LOG_DIR);
    if (!dir) {
      return;
    }
    int count = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && count < 8) {
      size_t full = strlen(e->d_name);
      if (strcasestr(e->d_name, ".fdb.") && full < sizeof(names[0])) {
        memcpy(names[count++], e->d_name, full + 1);
      }
    }
    closedir(dir);
    if (count == 0) {
      return;
    }
    for (int i = 0; i < count; i++) {
      char path[sizeof(LOG_DIR) + 34];
      snprintf(path, sizeof(path), LOG_DIR "/%s", names[i]);
      if (unlink(path) != 0) {
        return; /* don't loop on a file that stays */
      }
    }
  }
}

bool log_file_start(void) {
  if (!s_tail) {
    s_tail = heap_caps_malloc(TAIL_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (!s_tail) {
      return false;
    }
  }
  struct stat st;
  if (stat(LOG_DIR, &st) != 0 && mkdir(LOG_DIR, 0775) != 0) {
    return false; /* no card */
  }
  remove_fdb_files();
  return true;
}
