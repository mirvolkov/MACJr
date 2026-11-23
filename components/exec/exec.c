/*
 * Program loader: runs ELF programs from the SD card, one at a time, DOS
 * style. Espressif's elf_loader does the ELF work -- it copies the
 * sections into PSRAM, applies the relocations, binds the program's calls
 * to the symbols the firmware registered, and syncs the caches so that the
 * code can be fetched through the instruction bus alias of that PSRAM.
 *
 * A program gets the tty as the third argument of main() and draws on it
 * with the tty API itself; the rest of the API (sdk/include/macjr.h) is
 * the sys_* calls here and the firmware functions listed in exports.c,
 * looked up before the loader's libc table. exec_run() runs a program in
 * the caller's task, exec_spawn() in its own task on CPU1; either way
 * sys_exit() gets back out of it with longjmp().
 *
 * Standard streams: descriptors 0, 1 and 2 of a program are its tty, one
 * descriptor of /dev/tty (ttydev.c) opened for it, so write(1, ...) and
 * read(0, ...) reach the screen and the keys, and printf(), vprintf(),
 * puts() and putchar() print there (the firmware's own stdout stays the
 * serial console). close() of 0..2 does nothing.
 *
 * Killing (F and Cx together, while exec_spawn() waits): the program is
 * asked to end first. Its blocking calls -- read(), select(), the delays,
 * sys_getkey() -- wait in slices of KILL_POLL_MS and check for the
 * request in between, so a program that waits for something ends at its
 * next wait, through sys_exit(), and cleans up like any other. A program
 * that hasn't ended KILL_GRACE_MS later and isn't blocked in a firmware
 * call is computing in a loop: its task is deleted where it stands and
 * exec cleans up after it. Either way the program's descriptors are
 * closed and the ELF unloaded from PSRAM; exit code EXEC_KILLED.
 */
#include "exec.h"
#include "exec_exports.h"
#include "exec_priv.h"
#include "ttydev.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "private/elf_symbol.h"
#include "db.h"
#include "logdev.h"
#include "keyboard.h"
#include "macjr.h"
#include "mic.h"
#include "monitor.h"
#include "usbdev.h"

static const char *TAG = "exec";

#define KEY_MENU KEY_ESC /* Cx */

_Static_assert(SYS_KEY_ENTER == KEY_MK_ENTER && SYS_KEY_ESC == KEY_ESC,
               "macjr.h key codes out of sync with keyboard.h");

#define KILL_POLL_MS  50   /* slice of a blocking call between kill checks */
#define KILL_GRACE_MS 1000 /* before a program that doesn't wait is deleted */
#define MAX_FDS       16   /* descriptors a program may have open */

/* State of the running program: one at a time. */
static int s_kbd = -1;
static jmp_buf s_exit_jmp;
static int s_exit_status;
static volatile bool s_kill; /* end at the next blocking call */
static int s_fds[MAX_FDS];   /* the program's open descriptors, -1 = free */
static esp_elf_t *s_elf;     /* the program loaded, NULL for a built-in */
static bool s_card_taken;    /* it took the SD card away from the filesystem */
static int s_std = -1;       /* its descriptor of /dev/tty: what 0..2 stand for */
static TaskHandle_t s_task;  /* the task it runs in */

/* ---- descriptors and kill checks ------------------------------------ */

static void fds_reset(void) {
  for (int i = 0; i < MAX_FDS; i++) {
    s_fds[i] = -1;
  }
}

static void fd_track(int fd, bool open) {
  for (int i = 0; i < MAX_FDS; i++) {
    if (s_fds[i] == (open ? -1 : fd)) {
      s_fds[i] = open ? fd : -1;
      return;
    }
  }
  if (open) {
    ESP_LOGW(TAG, "more than %d descriptors: fd %d stays open after the program", MAX_FDS, fd);
  }
}

/* Close what the program left open. */
static void fds_close_all(void) {
  for (int i = 0; i < MAX_FDS; i++) {
    if (s_fds[i] >= 0) {
      close(s_fds[i]);
      s_fds[i] = -1;
    }
  }
}

/* A safe point: end the program if it is being killed. Program task only. */
static void kill_check(void) {
  if (s_kill) {
    sys_exit(EXEC_KILLED);
  }
}

/* Block until `fd` has data, checking for a kill meanwhile. Only for a
 * blocking character device (/dev/kbrd, /dev/db/...): files don't block,
 * and select() of a driver without select support just times out.
 * /dev/tty waits for its keys itself, checking for a kill. */
static void wait_readable(int fd) {
  struct stat st;
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || (flags & O_NONBLOCK) || fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode) ||
      st.st_rdev == TTY_RDEV) {
    return;
  }
  for (;;) {
    kill_check();
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval slice = {.tv_sec = 0, .tv_usec = KILL_POLL_MS * 1000};
    if (select(fd + 1, &set, NULL, NULL, &slice) != 0) {
      return; /* readable, or select() failed: read() reports it */
    }
  }
}

/* vTaskDelay() in slices, checking for a kill. */
static void delay_ticks(TickType_t ticks) {
  const TickType_t slice = pdMS_TO_TICKS(KILL_POLL_MS);
  for (;;) {
    kill_check();
    if (ticks <= slice) {
      vTaskDelay(ticks);
      return;
    }
    vTaskDelay(slice);
    ticks -= slice;
  }
}

int exec_open(const char *path, int flags, int mode) {
  int fd = open(path, flags, mode);
  if (fd >= 0) {
    fd_track(fd, true);
  }
  return fd;
}

/* A standard stream (0..2) of the program: its /dev/tty descriptor. */
static bool is_std(int fd) {
  return fd >= STDIN_FILENO && fd <= STDERR_FILENO && s_std >= 0;
}

int exec_close(int fd) {
  if (is_std(fd)) {
    return 0; /* the tty stays the program's */
  }
  fd_track(fd, false);
  return close(fd);
}

ssize_t exec_read(int fd, void *buf, size_t len) {
  if (is_std(fd)) {
    fd = s_std;
  }
  wait_readable(fd);
  return read(fd, buf, len);
}

ssize_t exec_write(int fd, const void *buf, size_t len) {
  return write(is_std(fd) ? s_std : fd, buf, len);
}

/* ---- stdio on the program's tty -------------------------------------- */

static int out(const char *s, size_t len) {
  return len == 0 || exec_write(STDOUT_FILENO, s, len) == (ssize_t)len ? 0 : -1;
}

int exec_vprintf(const char *fmt, va_list ap) {
  char small[128];
  char *buf = small;
  va_list again;
  va_copy(again, ap);
  int n = vsnprintf(small, sizeof(small), fmt, ap);
  if (n >= (int)sizeof(small)) {
    buf = malloc((size_t)n + 1);
    if (buf) {
      vsnprintf(buf, (size_t)n + 1, fmt, again);
    }
  }
  va_end(again);
  if (n < 0 || !buf) {
    return -1;
  }
  if (out(buf, (size_t)n) != 0) {
    n = -1;
  }
  if (buf != small) {
    free(buf);
  }
  return n;
}

int exec_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = exec_vprintf(fmt, ap);
  va_end(ap);
  return n;
}

int exec_puts(const char *s) {
  return out(s, strlen(s)) == 0 && out("\n", 1) == 0 ? 1 : EOF;
}

int exec_putchar(int c) {
  char ch = (char)c;
  return out(&ch, 1) == 0 ? (unsigned char)c : EOF;
}

int exec_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
  int64_t deadline = timeout ? esp_timer_get_time() + timeout->tv_sec * 1000000LL + timeout->tv_usec : 0;
  fd_set none;
  FD_ZERO(&none);
  for (;;) {
    kill_check();
    int64_t left = timeout ? deadline - esp_timer_get_time() : KILL_POLL_MS * 1000;
    if (left < 0) {
      left = 0;
    }
    struct timeval slice = {.tv_sec = 0, .tv_usec = (suseconds_t)(left < KILL_POLL_MS * 1000 ? left : KILL_POLL_MS * 1000)};
    fd_set r = readfds ? *readfds : none, w = writefds ? *writefds : none, e = exceptfds ? *exceptfds : none;
    int n = select(nfds, readfds ? &r : NULL, writefds ? &w : NULL, exceptfds ? &e : NULL, &slice);
    if (n != 0 || (timeout && left == 0)) {
      if (n >= 0) { /* the sets say what is ready (nothing on a timeout) */
        if (readfds) {
          *readfds = r;
        }
        if (writefds) {
          *writefds = w;
        }
        if (exceptfds) {
          *exceptfds = e;
        }
      }
      return n;
    }
  }
}

void exec_vTaskDelay(TickType_t ticks) {
  delay_ticks(ticks);
}

int exec_usleep(useconds_t us) {
  delay_ticks(pdMS_TO_TICKS((us + 999) / 1000));
  return 0;
}

unsigned exec_sleep(unsigned s) {
  delay_ticks(pdMS_TO_TICKS(s * 1000));
  return 0;
}

void exec_exit(int status) {
  sys_exit(status);
}

esp_err_t exec_sdcard_open_raw(sdmmc_card_t **out) {
  log_suspend(); /* its records go to the card first, then wait */
  db_close(); /* its files must not outlive the mount */
  s_card_taken = true;
  return sdcard_open_raw(out);
}

/* Give the card back to the filesystem and the db. */
static void card_restore(void) {
  if (!s_card_taken) {
    return;
  }
  s_card_taken = false;
  esp_err_t err = sdcard_close_raw();
  if (err == ESP_OK) {
    err = db_init();
    log_resume();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "SD card not restored: %s", esp_err_to_name(err));
  }
}

/* ---- sys_* API -------------------------------------------------------- */

/* The keys are the monitor's while it is open. */
static void wait_monitor_closed(void) {
  while (monitor_is_open()) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

int sys_getkey(void) {
  for (;;) {
    uint8_t code;
    if (s_kbd >= 0) {
      wait_readable(s_kbd);
    }
    if (s_kbd < 0 || read(s_kbd, &code, 1) != 1) {
      delay_ticks(pdMS_TO_TICKS(100));
      continue;
    }
    if (!monitor_is_open() && !KBD_IS_CHORD(code)) {
      return code;
    }
  }
}

int exec_tty_key(bool block) {
  if (xTaskGetCurrentTaskHandle() != s_task) {
    return -EPERM;
  }
  if (block) {
    return sys_getkey();
  }
  for (;;) { /* what is waiting, dropping what sys_getkey() would */
    kill_check();
    fd_set set;
    FD_ZERO(&set);
    if (s_kbd < 0) {
      return -EAGAIN;
    }
    FD_SET(s_kbd, &set);
    struct timeval now = {0};
    uint8_t code;
    if (select(s_kbd + 1, &set, NULL, NULL, &now) <= 0 || read(s_kbd, &code, 1) != 1) {
      return -EAGAIN;
    }
    if (!monitor_is_open() && !KBD_IS_CHORD(code)) {
      return code;
    }
  }
}

unsigned sys_millis(void) {
  return (unsigned)(esp_timer_get_time() / 1000);
}

void sys_delay(unsigned ms) {
  delay_ticks(pdMS_TO_TICKS(ms));
}

int sys_cpu(void) {
  return xPortGetCoreID();
}

void sys_exit(int status) {
  s_exit_status = status;
  longjmp(s_exit_jmp, 1);
}

/* ---- loader ----------------------------------------------------------- */

/* Read the whole file into a 4-byte aligned PSRAM buffer, as
 * esp_elf_relocate() wants it. */
static esp_err_t read_file(const char *path, uint8_t **out, size_t *out_size) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return ESP_ERR_NOT_FOUND;
  }
  esp_err_t ret = ESP_FAIL;
  uint8_t *buf = NULL;
  long size = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
  if (size > 0 && fseek(f, 0, SEEK_SET) == 0) {
    buf = heap_caps_aligned_alloc(4, size, MALLOC_CAP_SPIRAM);
    if (!buf) {
      ret = ESP_ERR_NO_MEM;
    } else if (fread(buf, 1, size, f) == (size_t)size) {
      ret = ESP_OK;
    }
  }
  fclose(f);
  if (ret != ESP_OK) {
    free(buf);
    return ret;
  }
  *out = buf;
  *out_size = (size_t)size;
  return ESP_OK;
}

/* Undo what the program may have left behind: descriptors, its keyboard
 * queue, callbacks into its code (which may be about to go), USB (back to
 * its default mode) and the SD card, in that order: the card comes back
 * only once the host can't reach it. */
static void program_cleanup(void) {
  fds_close_all();
  if (s_std >= 0) {
    close(s_std);
    s_std = -1;
  }
  ttydev_attach(NULL);
  s_task = NULL;
  if (s_kbd >= 0) {
    close(s_kbd);
    s_kbd = -1;
  }
  monitor_set_enabled(true);
  usbdev_stop();
  mic_uac_stop();
  card_restore();
}

/* Run `entry` in the calling task with a fresh keyboard queue and
 * sys_exit() armed; its exit code. */
static int run_main(macjr_tty_t *tty, macjr_main_t entry, int argc, char **argv) {
  fds_reset();
  s_kbd = open(KBD_DEV_PATH, O_RDONLY); /* fresh queue: no keys from before */
  s_task = xTaskGetCurrentTaskHandle();
  if (ttydev_init() == ESP_OK) {
    ttydev_attach(tty);
    s_std = open(TTY_DEV, O_RDWR); /* stdin, stdout, stderr */
  }
  if (setjmp(s_exit_jmp) == 0) {
    s_exit_status = entry(argc, argv, tty);
  }
  program_cleanup();
  return s_exit_status;
}

/* Symbols for programs: ours first, then the loader's tables. */
static uintptr_t resolve(const char *name) {
  for (const struct esp_elfsym *sym = g_exec_exports; sym->name; sym++) {
    if (strcmp(sym->name, name) == 0) {
      return (uintptr_t)sym->sym;
    }
  }
  return elf_find_sym_default(name);
}

esp_err_t exec_run(macjr_tty_t *tty, const char *path, int argc, char **argv, int *status) {
  elf_set_symbol_resolver(resolve);

  uint8_t *image;
  size_t size;
  esp_err_t ret = read_file(path, &image, &size);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "%s: %s", path, esp_err_to_name(ret));
    return ret;
  }

  esp_elf_t elf;
  int err = esp_elf_init(&elf);
  if (err == 0) {
    err = esp_elf_relocate(&elf, image, size);
  }
  free(image); /* the sections were copied out of it */
  if (err != 0) {
    ESP_LOGE(TAG, "%s: cannot load, error %d", path, err);
    esp_elf_deinit(&elf);
    return err == -ENOMEM ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
  }
  ESP_LOGI(TAG, "%s: %u bytes, main at %p, CPU%d", path, (unsigned)size, (void *)elf.entry, xPortGetCoreID());

  /* elf.entry directly, not esp_elf_request(): that one drops the
   * return value of main() and has no room for the tty. The loader types
   * the entry as main(argc, argv); ours takes the tty as well. */
  s_elf = &elf; /* for a forced kill, which unloads it instead */
  *status = run_main(tty, (macjr_main_t)(uintptr_t)elf.entry, argc, argv);
  s_elf = NULL;
  esp_elf_deinit(&elf);
  ESP_LOGI(TAG, "%s: exit code %d", path, *status);
  return ESP_OK;
}

typedef struct {
  macjr_tty_t *tty;
  const char *path;
  macjr_main_t entry; /* built-in program: run this instead of loading `path` */
  int argc;
  char **argv;
  int status;
  esp_err_t ret;
  TaskHandle_t waiter;
} spawn_job_t;

static void spawn_task(void *arg) {
  spawn_job_t *job = arg;
  if (job->entry) {
    job->status = run_main(job->tty, job->entry, job->argc, job->argv);
    job->ret = ESP_OK;
  } else {
    job->ret = exec_run(job->tty, job->path, job->argc, job->argv, &job->status);
  }
  xTaskNotifyGive(job->waiter);
  vTaskSuspend(NULL); /* spawn() deletes the task: its handle stays valid */
}

/* The program didn't end when asked: delete its task and clean up for
 * it. Its task is suspended first, so it can't end meanwhile. A lock it
 * held at that moment (inside a firmware call) stays taken: the price of
 * a program that never waits. */
static void force_kill(TaskHandle_t task, spawn_job_t *job) {
  vTaskSuspend(task);
  if (ulTaskNotifyTake(pdTRUE, 0)) {
    vTaskDelete(task); /* it ended after all */
    return;
  }
  ESP_LOGW(TAG, "program doesn't stop: deleting its task");
  if (s_elf) {
    esp_elf_deinit(s_elf); /* frees its PSRAM; `elf` lives on that task's stack */
    s_elf = NULL;
  }
  vTaskDelete(task);
  program_cleanup();
  job->status = EXEC_KILLED;
  job->ret = ESP_OK;
}

static esp_err_t spawn(macjr_tty_t *tty, const char *path, macjr_main_t entry, int argc, char **argv,
                       int *status) {
  spawn_job_t job = {
    .tty = tty,
    .path = path,
    .entry = entry,
    .argc = argc,
    .argv = argv,
    .waiter = xTaskGetCurrentTaskHandle(),
  };
  /* our own keyboard queue, for the kill chord */
  int kbd = open(KBD_DEV_PATH, O_RDONLY | O_NONBLOCK);
  s_kill = false;
  /* the task is named after the program ("SI5351_CONTROL"): the log
   * stamps records with it */
  char name[configMAX_TASK_NAME_LEN];
  const char *base = path ? path : argv[0];
  const char *slash = strrchr(base, '/');
  snprintf(name, sizeof(name), "%s", slash ? slash + 1 : base);
  char *dot = strrchr(name, '.');
  if (dot && dot != name) {
    *dot = '\0';
  }
  TaskHandle_t task;
  if (xTaskCreatePinnedToCore(spawn_task, name, EXEC_STACK_SIZE, &job, EXEC_PRIORITY, &task, EXEC_CORE) !=
      pdPASS) {
    if (kbd >= 0) {
      close(kbd);
    }
    return ESP_ERR_NO_MEM;
  }
  int64_t kill_us = 0; /* when the kill was asked for, 0 = not yet */
  for (;;) { /* job lives on this stack until the program ends */
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(KILL_POLL_MS))) {
      vTaskDelete(task);
      break;
    }
    uint8_t key;
    while (!kill_us && kbd >= 0 && read(kbd, &key, 1) == 1) {
      if (key == KEY_MK_F_ESC) {
        ESP_LOGW(TAG, "killing the program");
        kill_us = esp_timer_get_time();
        s_kill = true;
      }
    }
    if (kill_us && esp_timer_get_time() - kill_us > KILL_GRACE_MS * 1000LL && eTaskGetState(task) != eBlocked) {
      force_kill(task, &job);
      break;
    }
  }
  s_kill = false;
  if (kbd >= 0) {
    close(kbd);
  }
  *status = job.status;
  return job.ret;
}

esp_err_t exec_spawn(macjr_tty_t *tty, const char *path, int argc, char **argv, int *status) {
  return spawn(tty, path, NULL, argc, argv, status);
}

/* exec_launch() of the program at `path`, or of `entry` if set, with
 * `argv` (argv[0] is the name shown). */
static void launch(macjr_tty_t *tty, const char *path, macjr_main_t entry, int argc, char **argv) {
  const char *name = argv[0];
  const char *clear = "\x1b[0m\x1b[2J\x1b[H\x1b[?25l";
  macjr_tty_write(tty, clear, strlen(clear));
  macjr_display_flush(tty);

  size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  int status = 0;
  esp_err_t ret = spawn(tty, path, entry, argc, argv, &status);
  size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (heap_before != heap_after) {
    ESP_LOGW(TAG, "PSRAM free %u -> %u bytes", (unsigned)heap_before, (unsigned)heap_after);
  }

  if (ret == ESP_OK) {
    /* ended or killed: straight back to the menu (the exit code is in the log) */
    if (status == EXEC_KILLED) {
      ESP_LOGI(TAG, "%s killed", name);
    }
    wait_monitor_closed();
    return;
  }

  /* not loaded: the reason stays on screen until Cx */
  char msg[160];
  snprintf(msg, sizeof(msg), "\x1b[2J\x1b[H\x1b[31mCannot run %s:\r\n%s\x1b[0m\r\n", name,
           ret == ESP_ERR_INVALID_ARG ? "not a program, or needs a function\r\nthe firmware doesn't export"
                                      : esp_err_to_name(ret));
  wait_monitor_closed();
  macjr_tty_write(tty, msg, strlen(msg));
  const char *hint = "\x1b[90mPress Cx for the menu\x1b[0m";
  macjr_tty_write(tty, hint, strlen(hint));
  macjr_display_flush(tty);

  /* opened only now: keys the program read must not count here */
  int kbd = open(KBD_DEV_PATH, O_RDONLY);
  for (;;) {
    uint8_t code;
    if (kbd < 0 || read(kbd, &code, 1) != 1) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (!monitor_is_open() && code == KEY_MENU) {
      break;
    }
  }
  if (kbd >= 0) {
    close(kbd);
  }
}

static char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return (char *)(slash ? slash + 1 : path);
}

void exec_launch(macjr_tty_t *tty, const char *path) {
  char *argv[] = {base_name(path), NULL};
  launch(tty, path, NULL, 1, argv);
}

void exec_launch_with(macjr_tty_t *tty, const char *path, const char *arg) {
  char *argv[] = {base_name(path), (char *)arg, NULL};
  launch(tty, path, NULL, 2, argv);
}

void exec_launch_main(macjr_tty_t *tty, const char *name, macjr_main_t entry) {
  char *argv[] = {(char *)name, NULL};
  launch(tty, NULL, entry, 1, argv);
}
