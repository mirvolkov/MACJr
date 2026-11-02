/*
 * Change notification for the key-value store (see db_watch() in db.h):
 * the character device DB_WATCH_DEV. Every open() is one watch with its
 * own queue of "<key>\n" lines; the path after the device is the key
 * prefix it watches. select() reports a watch readable while its queue
 * has lines, as /dev/kbrd does for keys.
 *
 * A change that doesn't fit a watch's queue empties it and leaves a
 * single "*\n" line instead: changes were lost, read everything again.
 * Until the reader has emptied the queue, further changes add nothing.
 */
#include "db_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#define WATCH_QUEUE_BYTES 256 /* per watch */
#define WATCH_MAX_SELECTS 4   /* select() calls waiting at the same time */
#define WATCH_LOST        "*\n"

static const char *TAG = "db_watch";

typedef struct {
  StreamBufferHandle_t buf; /* NULL = slot free */
  bool nonblock;
  bool lost; /* the queue holds just WATCH_LOST, not read out yet */
  char prefix[DB_KEY_MAX + 1];
} watch_t;

/* A select() waiting on some of our descriptors. */
typedef struct {
  esp_vfs_select_sem_t sem;
  fd_set *readfds; /* the caller's set: ready descriptors go here */
  fd_set watched;  /* the descriptors it asked about */
} watch_select_t;

static watch_t s_watches[DB_MAX_WATCHES];
static watch_select_t *s_selects[WATCH_MAX_SELECTS];
static SemaphoreHandle_t s_lock; /* s_watches and s_selects */

static bool watch_valid(int fd) {
  return fd >= 0 && fd < DB_MAX_WATCHES && s_watches[fd].buf;
}

/* Wake the select() calls watching `fd`. Under s_lock. */
static void select_notify(int fd) {
  for (int i = 0; i < WATCH_MAX_SELECTS; i++) {
    watch_select_t *sel = s_selects[i];
    if (sel && FD_ISSET(fd, &sel->watched)) {
      FD_SET(fd, sel->readfds);
      esp_vfs_select_triggered(sel->sem);
    }
  }
}

/* Queue "<key>\n" on watch `w`, or the lost-changes line if it doesn't
 * fit. Under s_lock. */
static void queue_line(watch_t *w, const char *key) {
  if (w->lost) {
    return; /* the reader reads everything again anyway */
  }
  char line[DB_KEY_MAX + 2];
  int len = snprintf(line, sizeof(line), "%s\n", key);
  if (xStreamBufferSpacesAvailable(w->buf) >= (size_t)len) {
    xStreamBufferSend(w->buf, line, len, 0);
    return;
  }
  /* Full, so no reader is blocked on it and the reset succeeds. */
  xStreamBufferReset(w->buf);
  xStreamBufferSend(w->buf, WATCH_LOST, strlen(WATCH_LOST), 0);
  w->lost = true;
}

void db_watch_notify(const char *key) {
  if (!s_lock) {
    return;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int fd = 0; fd < DB_MAX_WATCHES; fd++) {
    watch_t *w = &s_watches[fd];
    if (w->buf && strncmp(key, w->prefix, strlen(w->prefix)) == 0) {
      queue_line(w, key);
      select_notify(fd);
    }
  }
  xSemaphoreGive(s_lock);
}

/* ---- DB_WATCH_DEV ------------------------------------------------------ */

static int watch_open(void *ctx, const char *path, int flags, int mode) {
  const char *prefix = path[0] == '/' ? path + 1 : path;
  if (strlen(prefix) > DB_KEY_MAX) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if ((flags & O_ACCMODE) != O_RDONLY) {
    errno = EACCES;
    return -1;
  }
  StreamBufferHandle_t buf = xStreamBufferCreate(WATCH_QUEUE_BYTES, 1);
  if (!buf) {
    errno = ENOMEM;
    return -1;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int fd = 0; fd < DB_MAX_WATCHES; fd++) {
    watch_t *w = &s_watches[fd];
    if (!w->buf) {
      w->buf = buf;
      w->nonblock = (flags & O_NONBLOCK) != 0;
      w->lost = false;
      strcpy(w->prefix, prefix);
      xSemaphoreGive(s_lock);
      return fd;
    }
  }
  xSemaphoreGive(s_lock);
  vStreamBufferDelete(buf);
  errno = ENFILE;
  return -1;
}

static ssize_t watch_read(void *ctx, int fd, void *dst, size_t size) {
  if (!watch_valid(fd)) {
    errno = EBADF;
    return -1;
  }
  if (size == 0) {
    return 0;
  }
  watch_t *w = &s_watches[fd];
  size_t n = xStreamBufferReceive(w->buf, dst, size, w->nonblock ? 0 : portMAX_DELAY);
  if (n == 0) {
    errno = EAGAIN;
    return -1;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (xStreamBufferBytesAvailable(w->buf) == 0) {
    w->lost = false; /* the "*" line is out: report changes again */
  }
  xSemaphoreGive(s_lock);
  return (ssize_t)n;
}

static int watch_close(void *ctx, int fd) {
  if (!watch_valid(fd)) {
    errno = EBADF;
    return -1;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  StreamBufferHandle_t buf = s_watches[fd].buf;
  s_watches[fd].buf = NULL;
  xSemaphoreGive(s_lock);
  vStreamBufferDelete(buf);
  return 0;
}

static int watch_fcntl(void *ctx, int fd, int cmd, int arg) {
  if (!watch_valid(fd)) {
    errno = EBADF;
    return -1;
  }
  switch (cmd) {
  case F_GETFL:
    return O_RDONLY | (s_watches[fd].nonblock ? O_NONBLOCK : 0);
  case F_SETFL:
    s_watches[fd].nonblock = (arg & O_NONBLOCK) != 0;
    return 0;
  default:
    errno = EINVAL;
    return -1;
  }
}

static int watch_fstat(void *ctx, int fd, struct stat *st) {
  if (!watch_valid(fd)) {
    errno = EBADF;
    return -1;
  }
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFCHR | 0444;
  return 0;
}

/* select(): readable while the watch has lines queued. Never writable,
 * no exceptions. The VFS hands us its sets with our own descriptor
 * numbers; they are cleared here and refilled as descriptors become
 * ready. */
static esp_err_t watch_start_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
                                    esp_vfs_select_sem_t sem, void **end_select_args) {
  *end_select_args = NULL;
  int max_fd = nfds < DB_MAX_WATCHES ? nfds : DB_MAX_WATCHES;
  watch_select_t *sel = calloc(1, sizeof(*sel));
  if (!sel) {
    return ESP_ERR_NO_MEM;
  }
  sel->sem = sem;
  sel->readfds = readfds;
  FD_ZERO(&sel->watched);

  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int fd = 0; fd < max_fd; fd++) {
    if (FD_ISSET(fd, readfds)) {
      if (!watch_valid(fd)) {
        xSemaphoreGive(s_lock);
        free(sel);
        return ESP_ERR_INVALID_STATE; /* select() fails with EBADF */
      }
      FD_SET(fd, &sel->watched);
    }
  }
  int slot = 0;
  while (slot < WATCH_MAX_SELECTS && s_selects[slot]) {
    slot++;
  }
  if (slot == WATCH_MAX_SELECTS) {
    xSemaphoreGive(s_lock);
    free(sel);
    return ESP_ERR_NO_MEM;
  }
  FD_ZERO(readfds);
  FD_ZERO(writefds);
  FD_ZERO(exceptfds);
  s_selects[slot] = sel;
  for (int fd = 0; fd < max_fd; fd++) {
    if (FD_ISSET(fd, &sel->watched) && xStreamBufferBytesAvailable(s_watches[fd].buf) > 0) {
      select_notify(fd); /* lines already waiting */
    }
  }
  xSemaphoreGive(s_lock);
  *end_select_args = sel;
  return ESP_OK;
}

static esp_err_t watch_end_select(void *end_select_args) {
  watch_select_t *sel = end_select_args;
  if (!sel) {
    return ESP_OK;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int i = 0; i < WATCH_MAX_SELECTS; i++) {
    if (s_selects[i] == sel) {
      s_selects[i] = NULL;
    }
  }
  xSemaphoreGive(s_lock);
  free(sel);
  return ESP_OK;
}

static const esp_vfs_select_ops_t s_watch_select_ops = {
  .start_select = watch_start_select,
  .end_select = watch_end_select,
};

static const esp_vfs_fs_ops_t s_watch_ops = {
  .open_p = watch_open,
  .read_p = watch_read,
  .close_p = watch_close,
  .fcntl_p = watch_fcntl,
  .fstat_p = watch_fstat,
  .select = &s_watch_select_ops,
};

esp_err_t db_watch_init(void) {
  if (s_lock) {
    return ESP_OK;
  }
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) {
    return ESP_ERR_NO_MEM;
  }
  esp_err_t err = esp_vfs_register_fs(DB_WATCH_DEV, &s_watch_ops, ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_STATIC,
                                      NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cannot register %s: %s", DB_WATCH_DEV, esp_err_to_name(err));
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
  }
  return err;
}

int db_watch(const char *prefix) {
  char path[sizeof(DB_WATCH_DEV) + 1 + DB_KEY_MAX + 1];
  if (!prefix || strlen(prefix) > DB_KEY_MAX) {
    errno = EINVAL;
    return -1;
  }
  snprintf(path, sizeof(path), DB_WATCH_DEV "/%s", prefix);
  return open(path, O_RDONLY);
}
