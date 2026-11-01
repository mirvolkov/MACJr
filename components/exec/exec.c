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
 * registered once as an extra symbol table next to the loader's libc one. exec_run()
 * runs a program in the caller's task, exec_spawn() in its own task on
 * CPU1; either way sys_exit() gets back out of it with longjmp().
 */
#include "exec.h"
#include "exec_exports.h"

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard.h"
#include "macjr.h"
#include "monitor.h"

static const char *TAG = "exec";

#define KEY_MENU KEY_MK_ENTER /* В↑ */

_Static_assert(SYS_KEY_ENTER == KEY_MK_ENTER, "macjr.h key codes out of sync with keyboard.h");

/* State of the running program: one at a time. */
static int s_kbd = -1;
static jmp_buf s_exit_jmp;
static int s_exit_status;

/* ---- sys_* API -------------------------------------------------------- */

/* The monitor owns the screen and the keys while it is open. */
static void wait_monitor_closed(void) {
  while (monitor_is_open()) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

int sys_getkey(void) {
  for (;;) {
    uint8_t code;
    if (s_kbd < 0 || read(s_kbd, &code, 1) != 1) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (!monitor_is_open() && code != MONITOR_KEY) {
      return code;
    }
  }
}

unsigned sys_millis(void) {
  return (unsigned)(esp_timer_get_time() / 1000);
}

void sys_delay(unsigned ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
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

/* Run `entry` in the calling task with a fresh keyboard queue and
 * sys_exit() armed; its exit code. */
static int run_main(macjr_tty_t *tty, macjr_main_t entry, int argc, char **argv) {
  s_kbd = open(KBD_DEV_PATH, O_RDONLY); /* fresh queue: no keys from before */
  if (setjmp(s_exit_jmp) == 0) {
    s_exit_status = entry(argc, argv, tty);
  }
  if (s_kbd >= 0) {
    close(s_kbd);
    s_kbd = -1;
  }
  /* undo what the program may have set: its code may be about to go */
  monitor_set_redraw_cb(NULL, NULL);
  monitor_set_enabled(true);
  return s_exit_status;
}

esp_err_t exec_run(macjr_tty_t *tty, const char *path, int argc, char **argv, int *status) {
  static bool registered;
  if (!registered) {
    int err = esp_elf_register_symbol(g_exec_exports);
    if (err != 0 && err != -EEXIST) {
      ESP_LOGE(TAG, "esp_elf_register_symbol: %d", err);
      return ESP_ERR_NO_MEM;
    }
    registered = true;
  }

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
  *status = run_main(tty, (macjr_main_t)(uintptr_t)elf.entry, argc, argv);
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
  vTaskDelete(NULL);
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
  if (xTaskCreatePinnedToCore(spawn_task, "prog", EXEC_STACK_SIZE, &job, EXEC_PRIORITY, NULL, EXEC_CORE) !=
      pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* job lives on this stack until then */
  *status = job.status;
  return job.ret;
}

esp_err_t exec_spawn(macjr_tty_t *tty, const char *path, int argc, char **argv, int *status) {
  return spawn(tty, path, NULL, argc, argv, status);
}

/* exec_launch() of the program at `path`, or of `entry` if set. */
static void launch(macjr_tty_t *tty, const char *name, const char *path, macjr_main_t entry) {
  const char *clear = "\x1b[0m\x1b[2J\x1b[H\x1b[?25l";
  macjr_tty_write(tty, clear, strlen(clear));
  macjr_display_flush(tty);

  char *argv[] = {(char *)name, NULL};
  size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  int status = 0;
  esp_err_t ret = spawn(tty, path, entry, 1, argv, &status);
  size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (heap_before != heap_after) {
    ESP_LOGW(TAG, "PSRAM free %u -> %u bytes", (unsigned)heap_before, (unsigned)heap_after);
  }

  char msg[160];
  if (ret == ESP_OK) {
    snprintf(msg, sizeof(msg), "\r\n\x1b[32m-- %s: exit code %d\x1b[0m\r\n", name, status);
  } else {
    snprintf(msg, sizeof(msg), "\x1b[2J\x1b[H\x1b[31mCannot run %s:\r\n%s\x1b[0m\r\n", name,
             ret == ESP_ERR_INVALID_ARG ? "not a program, or needs a function\r\nthe firmware doesn't export"
                                        : esp_err_to_name(ret));
  }
  wait_monitor_closed();
  macjr_tty_write(tty, msg, strlen(msg));
  const char *hint = "\x1b[90mPress \x82^ for the menu\x1b[0m"; /* В^ in CP866 */
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

void exec_launch(macjr_tty_t *tty, const char *path) {
  launch(tty, strrchr(path, '/') ? strrchr(path, '/') + 1 : path, path, NULL);
}

void exec_launch_main(macjr_tty_t *tty, const char *name, macjr_main_t entry) {
  launch(tty, name, NULL, entry);
}
