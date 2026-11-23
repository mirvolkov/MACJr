/*
 * The system log (see logdev.h): a ring of LOG_RING fixed-size records in
 * PSRAM, the character device LOG_DEV over it, and a task that saves new
 * records to the card (log_file.c) every LOG_FLUSH_MS.
 *
 * Record `id` lives in slot id % LOG_RING. A slot whose id doesn't match
 * is a gap (a record lost before it was saved, at a boot); readers skip
 * it. s_first is the oldest id kept, s_next the one the next record gets.
 * A reader's descriptor holds only the id it reads next.
 */
#include "logdev_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>

#include "devfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LOG_MAX_SELECTS 4  /* select() calls waiting at the same time */
#define FLUSH_BATCH     32 /* records saved per store call */
#define FLUSH_STACK     4096
#define FLUSH_PRIO      2

static const char *TAG = "log";

typedef struct {
  bool used;
  bool nonblock;
  bool lost; /* the next record read gets LOG_LOST */
  uint32_t next; /* id to read next */
  SemaphoreHandle_t wake; /* given when a record is added */
} log_fd_t;

typedef struct {
  esp_vfs_select_sem_t sem;
  fd_set *readfds;
  fd_set watched;
} log_select_t;

static log_rec_t *s_ring;
static uint32_t s_first = 1, s_next = 1;
static uint32_t s_saved; /* the newest id on the card */
static bool s_card;      /* the card is ours (not lent out); under s_flush_lock */
static SemaphoreHandle_t s_lock;       /* ring, ids, descriptors, selects */
static SemaphoreHandle_t s_flush_lock; /* one flush at a time */
static log_fd_t s_fds[LOG_MAX_FDS];
static log_select_t *s_selects[LOG_MAX_SELECTS];
static log_rec_t *s_batch; /* FLUSH_BATCH records on their way to the card */

char log_level_char(int level) {
  return level >= 0 && level < (int)sizeof(LOG_LEVELS) - 1 ? LOG_LEVELS[level] : '?';
}

static void copy_name(char *dst, const char *src) {
  snprintf(dst, LOG_NAME_MAX, "%s", src ? src : "");
}

/* ---- the ring ---------------------------------------------------------- */

/* Readable: the descriptor has records waiting. Under s_lock. */
static bool waiting(const log_fd_t *f) {
  return f->next < s_next;
}

/* Put `rec` (its id set) into its slot. Under s_lock. */
static void put(const log_rec_t *rec) {
  memcpy(&s_ring[rec->id % LOG_RING], rec, LOG_REC_SIZE(rec));
  if (rec->id >= s_next) {
    s_next = rec->id + 1;
  }
  if (s_next - s_first > LOG_RING) {
    s_first = s_next - LOG_RING;
  }
}

/* Wake the readers and the select() calls. Under s_lock. */
static void notify(void) {
  for (int fd = 0; fd < LOG_MAX_FDS; fd++) {
    if (s_fds[fd].used) {
      xSemaphoreGive(s_fds[fd].wake);
    }
  }
  for (int i = 0; i < LOG_MAX_SELECTS; i++) {
    log_select_t *sel = s_selects[i];
    if (!sel) {
      continue;
    }
    for (int fd = 0; fd < LOG_MAX_FDS; fd++) {
      if (FD_ISSET(fd, &sel->watched) && s_fds[fd].used && waiting(&s_fds[fd])) {
        FD_SET(fd, sel->readfds);
        esp_vfs_select_triggered(sel->sem);
      }
    }
  }
}

/* A new record from the current task. */
static int add(int level, const char *tag, const char *text, size_t len) {
  if (!s_ring) {
    return -1;
  }
  log_rec_t rec;
  if (len > LOG_BODY_MAX - 1) {
    len = LOG_BODY_MAX - 1;
  }
  while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
    len--;
  }
  memset(&rec, 0, LOG_REC_HEAD);
  rec.level = level >= LOG_ERROR && level <= LOG_DEBUG ? level : LOG_INFO;
  rec.len = (uint16_t)len;
  copy_name(rec.app, pcTaskGetName(NULL));
  copy_name(rec.tag, tag);
  memcpy(rec.body, text, len);
  rec.body[len] = '\0';

  xSemaphoreTake(s_lock, portMAX_DELAY);
  rec.id = s_next;
  put(&rec);
  notify();
  xSemaphoreGive(s_lock);
  return 0;
}

int log_vwrite(int level, const char *tag, const char *fmt, va_list ap) {
  char text[LOG_BODY_MAX];
  int n = vsnprintf(text, sizeof(text), fmt ? fmt : "", ap);
  if (n < 0) {
    return -1;
  }
  return add(level, tag, text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
}

int log_write(int level, const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = log_vwrite(level, tag, fmt, ap);
  va_end(ap);
  return r;
}

/* write(): "<L>tag: text", or plain text. */
static int add_raw(const char *buf, size_t size) {
  int level = LOG_INFO;
  char tag[LOG_NAME_MAX] = "";
  if (size >= 3 && buf[0] == '<' && buf[2] == '>' && buf[1] && strchr(LOG_LEVELS, buf[1])) {
    level = (int)(strchr(LOG_LEVELS, buf[1]) - LOG_LEVELS);
    buf += 3;
    size -= 3;
    size_t i = 0;
    while (i < size && i < LOG_NAME_MAX - 1 && buf[i] != ':' && buf[i] != ' ') {
      i++;
    }
    if (i > 0 && i + 1 < size && buf[i] == ':' && buf[i + 1] == ' ') {
      memcpy(tag, buf, i);
      tag[i] = '\0';
      buf += i + 2;
      size -= i + 2;
    }
  }
  return add(level, tag, buf, size);
}

/* ---- saving to the card ------------------------------------------------ */

/* Save what is waiting, unless the card is away. Under s_flush_lock. */
static void flush_locked(void) {
  while (s_card) {
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t id = s_saved + 1 > s_first ? s_saved + 1 : s_first;
    for (; id < s_next && n < FLUSH_BATCH; id++) {
      const log_rec_t *rec = &s_ring[id % LOG_RING];
      if (rec->id == id) {
        memcpy(&s_batch[n++], rec, LOG_REC_SIZE(rec));
      }
    }
    xSemaphoreGive(s_lock);
    if (n == 0) {
      break;
    }
    uint32_t last = log_file_append(s_batch, n);
    if (last == 0) {
      break; /* failed: try again at the next flush */
    }
    s_saved = last;
  }
}

void log_flush(void) {
  if (!s_ring) {
    return;
  }
  xSemaphoreTake(s_flush_lock, portMAX_DELAY);
  flush_locked();
  xSemaphoreGive(s_flush_lock);
}

void log_suspend(void) {
  if (!s_ring) {
    return;
  }
  xSemaphoreTake(s_flush_lock, portMAX_DELAY);
  flush_locked();
  s_card = false;
  xSemaphoreGive(s_flush_lock);
}

void log_resume(void) {
  if (!s_ring) {
    return;
  }
  xSemaphoreTake(s_flush_lock, portMAX_DELAY);
  s_card = log_file_start();
  xSemaphoreGive(s_flush_lock);
}

static void flush_task(void *arg) {
  (void)arg;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(LOG_FLUSH_MS));
    log_flush();
  }
}

/* Records back from the card at boot (log_file_load), in any order. */
static void loaded(const log_rec_t *rec, void *ctx) {
  (void)ctx;
  if (rec->id >= s_first && rec->id < s_next) {
    memcpy(&s_ring[rec->id % LOG_RING], rec, LOG_REC_SIZE(rec));
  }
}

/* ---- LOG_DEV ----------------------------------------------------------- */

static bool fd_valid(int fd) {
  return fd >= 0 && fd < LOG_MAX_FDS && s_fds[fd].used;
}

static int dev_open(void *ctx, const char *path, int flags, int mode) {
  if (path[0] && strcmp(path, "/") != 0) {
    errno = ENOENT;
    return -1;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int fd = 0; fd < LOG_MAX_FDS; fd++) {
    log_fd_t *f = &s_fds[fd];
    if (!f->used) {
      if (!f->wake) {
        f->wake = xSemaphoreCreateBinary();
        if (!f->wake) {
          break;
        }
      }
      xSemaphoreTake(f->wake, 0);
      f->used = true;
      f->nonblock = (flags & O_NONBLOCK) != 0;
      f->lost = false;
      f->next = s_first;
      xSemaphoreGive(s_lock);
      return fd;
    }
  }
  xSemaphoreGive(s_lock);
  errno = ENFILE;
  return -1;
}

static ssize_t dev_write(void *ctx, int fd, const void *buf, size_t size) {
  if (!fd_valid(fd)) {
    errno = EBADF;
    return -1;
  }
  if (add_raw(buf, size) != 0) {
    errno = EIO;
    return -1;
  }
  return (ssize_t)size;
}

static ssize_t dev_read(void *ctx, int fd, void *dst, size_t size) {
  if (!fd_valid(fd)) {
    errno = EBADF;
    return -1;
  }
  log_fd_t *f = &s_fds[fd];
  for (;;) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (f->next < s_first) {
      f->next = s_first;
      f->lost = true;
    }
    while (f->next < s_next && s_ring[f->next % LOG_RING].id != f->next) {
      f->next++; /* a gap */
    }
    if (f->next < s_next) {
      const log_rec_t *rec = &s_ring[f->next % LOG_RING];
      size_t n = LOG_REC_SIZE(rec);
      if (size < n) {
        xSemaphoreGive(s_lock);
        errno = EINVAL; /* records are read whole */
        return -1;
      }
      memcpy(dst, rec, n);
      if (f->lost) {
        ((log_rec_t *)dst)->flags |= LOG_LOST;
        f->lost = false;
      }
      f->next++;
      xSemaphoreGive(s_lock);
      return (ssize_t)n;
    }
    xSemaphoreGive(s_lock);
    if (f->nonblock) {
      errno = EAGAIN;
      return -1;
    }
    xSemaphoreTake(f->wake, portMAX_DELAY);
  }
}

static off_t dev_lseek(void *ctx, int fd, off_t off, int whence) {
  if (!fd_valid(fd) || off != 0) {
    errno = fd_valid(fd) ? EINVAL : EBADF;
    return -1;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  log_fd_t *f = &s_fds[fd];
  if (whence == SEEK_SET) {
    f->next = s_first;
  } else if (whence == SEEK_END) {
    f->next = s_next;
  } else if (whence != SEEK_CUR) {
    xSemaphoreGive(s_lock);
    errno = EINVAL;
    return -1;
  }
  f->lost = false;
  off_t at = (off_t)f->next;
  xSemaphoreGive(s_lock);
  return at;
}

static int dev_close(void *ctx, int fd) {
  if (!fd_valid(fd)) {
    errno = EBADF;
    return -1;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_fds[fd].used = false;
  xSemaphoreGive(s_lock);
  return 0;
}

static int dev_fcntl(void *ctx, int fd, int cmd, int arg) {
  if (!fd_valid(fd)) {
    errno = EBADF;
    return -1;
  }
  switch (cmd) {
  case F_GETFL:
    return O_RDWR | (s_fds[fd].nonblock ? O_NONBLOCK : 0);
  case F_SETFL:
    s_fds[fd].nonblock = (arg & O_NONBLOCK) != 0;
    return 0;
  default:
    errno = EINVAL;
    return -1;
  }
}

static int dev_fstat(void *ctx, int fd, struct stat *st) {
  if (!fd_valid(fd)) {
    errno = EBADF;
    return -1;
  }
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFCHR | 0666;
  return 0;
}

/* select(): readable while records are waiting; always writable. */
static esp_err_t dev_start_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
                                  esp_vfs_select_sem_t sem, void **end_select_args) {
  *end_select_args = NULL;
  int max_fd = nfds < LOG_MAX_FDS ? nfds : LOG_MAX_FDS;
  log_select_t *sel = calloc(1, sizeof(*sel));
  if (!sel) {
    return ESP_ERR_NO_MEM;
  }
  sel->sem = sem;
  sel->readfds = readfds;
  FD_ZERO(&sel->watched);
  fd_set want_write;
  FD_ZERO(&want_write);

  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int fd = 0; fd < max_fd; fd++) {
    bool r = FD_ISSET(fd, readfds), w = FD_ISSET(fd, writefds);
    if ((r || w) && !fd_valid(fd)) {
      xSemaphoreGive(s_lock);
      free(sel);
      return ESP_ERR_INVALID_STATE;
    }
    if (r) {
      FD_SET(fd, &sel->watched);
    }
    if (w) {
      FD_SET(fd, &want_write);
    }
  }
  int slot = 0;
  while (slot < LOG_MAX_SELECTS && s_selects[slot]) {
    slot++;
  }
  if (slot == LOG_MAX_SELECTS) {
    xSemaphoreGive(s_lock);
    free(sel);
    return ESP_ERR_NO_MEM;
  }
  FD_ZERO(readfds);
  FD_ZERO(writefds);
  FD_ZERO(exceptfds);
  s_selects[slot] = sel;
  bool ready = false;
  for (int fd = 0; fd < max_fd; fd++) {
    if (FD_ISSET(fd, &sel->watched) && waiting(&s_fds[fd])) {
      FD_SET(fd, readfds);
      ready = true;
    }
    if (FD_ISSET(fd, &want_write)) {
      FD_SET(fd, writefds);
      ready = true;
    }
  }
  if (ready) {
    esp_vfs_select_triggered(sem);
  }
  xSemaphoreGive(s_lock);
  *end_select_args = sel;
  return ESP_OK;
}

static esp_err_t dev_end_select(void *end_select_args) {
  log_select_t *sel = end_select_args;
  if (!sel) {
    return ESP_OK;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (int i = 0; i < LOG_MAX_SELECTS; i++) {
    if (s_selects[i] == sel) {
      s_selects[i] = NULL;
    }
  }
  xSemaphoreGive(s_lock);
  free(sel);
  return ESP_OK;
}

static const esp_vfs_select_ops_t s_select_ops = {
  .start_select = dev_start_select,
  .end_select = dev_end_select,
};

static const esp_vfs_fs_ops_t s_dev_ops = {
  .open_p = dev_open,
  .read_p = dev_read,
  .write_p = dev_write,
  .lseek_p = dev_lseek,
  .close_p = dev_close,
  .fcntl_p = dev_fcntl,
  .fstat_p = dev_fstat,
  .select = &s_select_ops,
};

/* ---- start, helpers ------------------------------------------------------ */

esp_err_t log_init(void) {
  if (s_ring) {
    return ESP_OK;
  }
  s_lock = xSemaphoreCreateMutex();
  s_flush_lock = xSemaphoreCreateMutex();
  log_rec_t *ring = heap_caps_calloc(LOG_RING, sizeof(log_rec_t), MALLOC_CAP_SPIRAM);
  s_batch = heap_caps_malloc(FLUSH_BATCH * sizeof(log_rec_t), MALLOC_CAP_SPIRAM);
  if (!s_lock || !s_flush_lock || !ring || !s_batch) {
    ESP_LOGE(TAG, "no memory for the log");
    heap_caps_free(ring);
    heap_caps_free(s_batch);
    s_batch = NULL;
    return ESP_ERR_NO_MEM;
  }
  s_ring = ring;
  s_card = log_file_start();
  if (s_card) {
    /* the numbers go on from the card's, its newest records come back */
    uint32_t last = log_file_last();
    s_saved = last;
    s_next = last + 1;
    s_first = last >= LOG_RING ? last - LOG_RING + 1 : 1;
    log_file_load(s_first, loaded, NULL);
  }
  esp_err_t err = esp_vfs_register_fs(LOG_DEV, &s_dev_ops, ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_STATIC, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cannot register %s: %s", LOG_DEV, esp_err_to_name(err));
    s_ring = NULL;
    heap_caps_free(ring);
    return err;
  }
  devfs_add("log", false);
  if (xTaskCreate(flush_task, "logflush", FLUSH_STACK, NULL, FLUSH_PRIO, NULL) != pdPASS) {
    ESP_LOGW(TAG, "no flush task: records stay in memory");
  }
  ESP_LOGI(TAG, "%s up: %u records kept, next #%lu", LOG_DEV, (unsigned)(s_next - s_first),
           (unsigned long)s_next);
  return ESP_OK;
}
