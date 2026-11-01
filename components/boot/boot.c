/*
 * Boot menu: the programs found in /BIN on the SD card, written to the
 * tty.
 *
 *   ═════════════ MacJr 0.0.2 ══════════════
 *
 *   1 - REMOTE
 *   2 - HELLO
 *   3 - MK52
 *   ...
 *
 *   Select: press 1-7
 *
 * Programs built into the firmware come first (in cyan), then the files,
 * listed by file name without ".ELF", sorted, with '_' shown as a space
 * (IDE_DEMO.ELF is "IDE DEMO").
 */
#include "boot.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "exec.h"
#include "keyboard.h"

static const char *TAG = "boot";

#define FIRST_ROW 3
#define NOTE_ROW 13
#define HINT_ROW 15
#define NAME_LEN 13 /* 8.3 name + NUL */

static char s_names[BOOT_MAX_ENTRIES][NAME_LEN];
static int s_count; /* programs in s_names */
static const boot_builtin_t *s_builtins;
static int s_n_builtins; /* listed before s_names */

static void out(macjr_tty_t *tty, const char *s) {
  macjr_tty_write(tty, s, strlen(s));
}

static int by_name(const void *a, const void *b) {
  return strcasecmp(a, b);
}

/* Fill s_names with the *.ELF files of EXEC_BIN_DIR (without ".ELF"), as
 * many as fit next to the built-in programs. False if the directory can't
 * be read. */
static bool scan_programs(void) {
  s_count = 0;
  DIR *dir = opendir(EXEC_BIN_DIR);
  if (!dir) {
    return false;
  }
  struct dirent *de;
  while ((de = readdir(dir)) != NULL && s_n_builtins + s_count < BOOT_MAX_ENTRIES) {
    size_t len = strlen(de->d_name);
    /* dot files: "._NAME.ELF" is macOS metadata, not a program */
    if (de->d_type == DT_DIR || de->d_name[0] == '.' || len <= 4 || len - 4 >= NAME_LEN ||
        strcasecmp(de->d_name + len - 4, ".ELF") != 0) {
      continue;
    }
    memcpy(s_names[s_count], de->d_name, len - 4);
    s_names[s_count][len - 4] = '\0';
    for (char *c = s_names[s_count]; *c; c++) {
      if (*c == '_') {
        *c = ' '; /* IDE_DEMO.ELF shows as "IDE DEMO" */
      }
    }
    s_count++;
  }
  closedir(dir);
  qsort(s_names, s_count, NAME_LEN, by_name);
  return true;
}

static void draw_menu(macjr_tty_t *tty, bool have_bin) {
  char line[96];
  out(tty, "\x1b[?25l\x1b[0m\x1b[2J");
  snprintf(line, sizeof(line), "MacJr %s", esp_app_get_description()->version);
  macjr_tty_header(tty, line);

  for (int i = 0; i < s_n_builtins; i++) {
    snprintf(line, sizeof(line), "\x1b[%d;1H\x1b[96m %d - %s", FIRST_ROW + i, i + 1, s_builtins[i].name);
    out(tty, line);
  }
  out(tty, "\x1b[97m");
  for (int i = 0; i < s_count; i++) {
    int entry = s_n_builtins + i;
    snprintf(line, sizeof(line), "\x1b[%d;1H %d - %s", FIRST_ROW + entry, entry + 1, s_names[i]);
    out(tty, line);
  }
  int total = s_n_builtins + s_count;
  if (s_count == 0) {
    snprintf(line, sizeof(line), "\x1b[%d;1H\x1b[91m %s %s", NOTE_ROW,
             have_bin ? "No programs in" : "Cannot read", EXEC_BIN_DIR + strlen(SDCARD_MOUNT_POINT));
    out(tty, line);
  }
  if (total == 0) {
    snprintf(line, sizeof(line), "\x1b[%d;1H\x1b[90m Press any key to look again\x1b[0m", HINT_ROW);
  } else {
    snprintf(line, sizeof(line), "\x1b[%d;1H\x1b[90m Select: press 1-%d\x1b[0m", HINT_ROW, total);
  }
  out(tty, line);
  macjr_display_flush(tty);
}

/* Menu entry (0-based) of a digit key, or -1. */
static int entry_of(uint8_t key) {
  static const uint8_t digits[BOOT_MAX_ENTRIES] = {KEY_MK_1, KEY_MK_2, KEY_MK_3, KEY_MK_4, KEY_MK_5,
                                                   KEY_MK_6, KEY_MK_7, KEY_MK_8, KEY_MK_9};
  for (int i = 0; i < BOOT_MAX_ENTRIES; i++) {
    if (digits[i] == key) {
      return i;
    }
  }
  return -1;
}

/* Wait for a digit key that picks a listed program (0-based), or -1 for
 * any key when there are none. */
static int wait_choice(int fd) {
  for (;;) {
    uint8_t key;
    if (fd < 0 || read(fd, &key, 1) != 1) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (s_n_builtins + s_count == 0) {
      return -1;
    }
    int entry = entry_of(key);
    if (entry >= 0 && entry < s_n_builtins + s_count) {
      return entry;
    }
    ESP_LOGI(TAG, "key %u is not a menu key", key);
  }
}

void boot_select(macjr_tty_t *tty, const boot_builtin_t *builtins, int n_builtins, char *path, size_t path_len,
                 const boot_builtin_t **builtin) {
  s_builtins = builtins;
  s_n_builtins = n_builtins < BOOT_MAX_ENTRIES ? n_builtins : BOOT_MAX_ENTRIES;
  int fd = open(KBD_DEV_PATH, O_RDONLY);
  if (fd < 0) {
    ESP_LOGE(TAG, "open %s failed", KBD_DEV_PATH);
  }
  int entry;
  do {
    bool have_bin = scan_programs();
    draw_menu(tty, have_bin);
    entry = wait_choice(fd);
  } while (entry < 0);
  if (fd >= 0) {
    close(fd);
  }
  if (entry < s_n_builtins) {
    *builtin = &s_builtins[entry];
    ESP_LOGI(TAG, "%s (built in) selected", s_builtins[entry].name);
    return;
  }
  *builtin = NULL;
  entry -= s_n_builtins;
  snprintf(path, path_len, "%s/%s.ELF", EXEC_BIN_DIR, s_names[entry]);
  for (char *c = path + strlen(EXEC_BIN_DIR); *c; c++) {
    if (*c == ' ') {
      *c = '_'; /* back to the file name */
    }
  }
  ESP_LOGI(TAG, "%s selected", path);
}
