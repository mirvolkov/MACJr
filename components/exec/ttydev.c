/*
 * /dev/tty: the running program's terminal as a file (ttydev.h).
 *
 * The device has no tty of its own: exec attaches the program's tty while
 * a program runs and detaches it when it ends, so every descriptor of
 * TTY_DEV reaches the screen of the program that is running, as /dev/tty
 * on Unix reaches the process's controlling terminal. Between programs
 * open() fails with ENXIO and the descriptors still open fail with EIO.
 *
 * write(): the bytes go through the tty (ANSI escapes, CP866, LF -> CRLF)
 * and the screen is flushed, so each write shows up at once.
 * read(): the keys, one key code per byte (keyboard.h), as sys_getkey()
 * gives them; only from the program's own task (the wait checks for a
 * kill and ends the program from there). Blocks until a key is pressed,
 * or fails with EAGAIN under O_NONBLOCK. No select() support.
 */
#include "ttydev.h"
#include "exec_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "devfs.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"

#define TTY_MAX_FDS 8

static const char *TAG = "ttydev";

typedef struct {
  bool used;
  bool nonblock;
} tty_fd_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static macjr_tty_t *volatile s_tty; /* the program's, NULL between programs */
static tty_fd_t s_fds[TTY_MAX_FDS];
static bool s_registered;

static bool fd_valid(int fd) {
  return fd >= 0 && fd < TTY_MAX_FDS && s_fds[fd].used;
}

static int dev_open(void *ctx, const char *path, int flags, int mode) {
  (void)ctx;
  (void)mode;
  if (path[0] && strcmp(path, "/") != 0) {
    errno = ENOENT;
    return -1;
  }
  if (!s_tty) {
    errno = ENXIO;
    return -1;
  }
  taskENTER_CRITICAL(&s_mux);
  for (int fd = 0; fd < TTY_MAX_FDS; fd++) {
    if (!s_fds[fd].used) {
      s_fds[fd].used = true;
      s_fds[fd].nonblock = (flags & O_NONBLOCK) != 0;
      taskEXIT_CRITICAL(&s_mux);
      return fd;
    }
  }
  taskEXIT_CRITICAL(&s_mux);
  errno = ENFILE;
  return -1;
}

static ssize_t dev_write(void *ctx, int fd, const void *buf, size_t size) {
  (void)ctx;
  macjr_tty_t *tty = s_tty;
  if (!fd_valid(fd) || !tty) {
    errno = fd_valid(fd) ? EIO : EBADF;
    return -1;
  }
  if (size == 0) {
    return 0;
  }
  macjr_tty_write(tty, buf, size);
  macjr_display_flush(tty);
  return (ssize_t)size;
}

static ssize_t dev_read(void *ctx, int fd, void *dst, size_t size) {
  (void)ctx;
  if (!fd_valid(fd) || !s_tty) {
    errno = fd_valid(fd) ? EIO : EBADF;
    return -1;
  }
  if (size == 0) {
    return 0;
  }
  int key = exec_tty_key(!s_fds[fd].nonblock);
  if (key < 0) {
    errno = key == -EAGAIN ? EAGAIN : EIO;
    return -1;
  }
  *(unsigned char *)dst = (unsigned char)key;
  return 1;
}

static int dev_close(void *ctx, int fd) {
  (void)ctx;
  if (!fd_valid(fd)) {
    errno = EBADF;
    return -1;
  }
  taskENTER_CRITICAL(&s_mux);
  s_fds[fd].used = false;
  taskEXIT_CRITICAL(&s_mux);
  return 0;
}

static int dev_fcntl(void *ctx, int fd, int cmd, int arg) {
  (void)ctx;
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
  (void)ctx;
  if (!fd_valid(fd)) {
    errno = EBADF;
    return -1;
  }
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFCHR | 0666;
  st->st_rdev = TTY_RDEV;
  return 0;
}

static const esp_vfs_fs_ops_t s_dev_ops = {
  .open_p = dev_open,
  .read_p = dev_read,
  .write_p = dev_write,
  .close_p = dev_close,
  .fcntl_p = dev_fcntl,
  .fstat_p = dev_fstat,
};

/* Descriptors 0..2 stand for the program's tty (exec.c), so the VFS must
 * never give them to a file: the console holds only some of them
 * (picolibc opens two), the rest get a console descriptor of their own. */
static void reserve_std_fds(void) {
  for (;;) {
    int fd = open("/dev/console", O_RDWR);
    if (fd < 0) {
      ESP_LOGW(TAG, "no /dev/console: descriptors 0..2 may go to files");
      return;
    }
    if (fd > STDERR_FILENO) {
      close(fd);
      return;
    }
  }
}

esp_err_t ttydev_init(void) {
  if (s_registered) {
    return ESP_OK;
  }
  reserve_std_fds();
  esp_err_t err = esp_vfs_register_fs(TTY_DEV, &s_dev_ops, ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_STATIC, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "cannot register %s: %s", TTY_DEV, esp_err_to_name(err));
    return err;
  }
  s_registered = true;
  devfs_add("tty", false);
  return ESP_OK;
}

void ttydev_attach(macjr_tty_t *tty) {
  s_tty = tty;
}
