/*
 * USBDRIVE.ELF -- the SD card as a USB drive. The ESP32 and the host must never both write the FAT on
 * the card, so the root directory is read first, the card is unmounted and
 * reopened raw, and only then handed to the host. The file list on screen
 * is therefore the card as it was when the mode started.
 *
 *   ═══════════[MacJr USB drive]════════════
 *
 *   Card   USD  14.9 GB
 *   Host   connected
 *   Read   1.2 MB   Write  0.0 MB
 *
 *   Files on the card:
 *   music                              <DIR>
 *   notes.txt                          1.2 KB
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "macjr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "monitor.h"
#include "sdcard.h"
#include "usbdev.h"

#define MAX_FILES 6        /* rows 8-13 */
#define NAME_COLS 28
#define REFRESH_MS 300

static const char *TAG = "usbdrive";

static macjr_tty_t *s_tty;
static volatile bool s_redraw; /* set when the monitor closes */

typedef struct {
  char name[NAME_COLS + 1];
  bool dir;
  uint32_t size;
} entry_t;

static entry_t s_files[MAX_FILES];
static int s_shown;
static int s_total;

static void out(const char *s) {
  macjr_tty_write(s_tty, s, strlen(s));
}

/* Move to row r (1-based), print the formatted text, clear the rest. */
static void line(int r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void line(int r, const char *fmt, ...) {
  char buf[96];
  int n = snprintf(buf, sizeof(buf), "\x1b[%d;1H", r);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  out(buf);
  out("\x1b[K");
}

static void human_size(uint64_t bytes, char *buf, size_t len) {
  if (bytes < 1024) {
    snprintf(buf, len, "%u B", (unsigned)bytes);
  } else if (bytes < 1024 * 1024) {
    snprintf(buf, len, "%.1f KB", bytes / 1024.0);
  } else if (bytes < 1024ull * 1024 * 1024) {
    snprintf(buf, len, "%.1f MB", bytes / (1024.0 * 1024));
  } else {
    snprintf(buf, len, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
  }
}

/* Read the root directory while FAT is still mounted. */
static void list_root(void) {
  DIR *dir = opendir(SDCARD_MOUNT_POINT);
  if (!dir) {
    s_total = -1;
    return;
  }
  struct dirent *de;
  while ((de = readdir(dir)) != NULL) {
    if (s_shown < MAX_FILES) {
      entry_t *e = &s_files[s_shown++];
      snprintf(e->name, sizeof(e->name), "%.*s", NAME_COLS, de->d_name); /* cut to fit */
      e->dir = de->d_type == DT_DIR;
      char path[300];
      struct stat st;
      snprintf(path, sizeof(path), SDCARD_MOUNT_POINT "/%s", de->d_name);
      e->size = (!e->dir && stat(path, &st) == 0) ? (uint32_t)st.st_size : 0;
    }
    s_total++;
  }
  closedir(dir);
}

static void draw_static(const sdmmc_card_t *card) {
  out("\x1b[?25l\x1b[0m\x1b[2J");
  macjr_tty_header(s_tty, "MacJr USB drive");

  if (card) {
    char cap[16];
    human_size((uint64_t)card->csd.capacity * card->csd.sector_size, cap, sizeof(cap));
    line(3, "\x1b[96mCard   \x1b[97m%.8s  %s", card->cid.name, cap);
  } else {
    line(3, "\x1b[96mCard   \x1b[91mnot available");
  }

  if (s_total < 0) {
    line(7, "\x1b[96mFiles: \x1b[97mcard was not mounted");
  } else {
    line(7, "\x1b[96mFiles on the card:");
    for (int i = 0; i < s_shown; i++) {
      char size[16];
      if (s_files[i].dir) {
        snprintf(size, sizeof(size), "<DIR>");
      } else {
        human_size(s_files[i].size, size, sizeof(size));
      }
      line(8 + i, "\x1b[97m%-*.*s %10s", NAME_COLS, NAME_COLS, s_files[i].name, size);
    }
    if (s_total > s_shown) {
      line(8 + MAX_FILES, "\x1b[90m... and %d more", s_total - s_shown);
    } else if (s_total == 0) {
      line(8, "\x1b[90m(empty)");
    }
  }
  line(15, "\x1b[90m Eject on the host before unplugging");
}

static void draw_status(const usbdev_msc_status_t *st, bool usb_ok) {
  char rd[16], wr[16];
  if (!usb_ok) {
    line(4, "\x1b[96mHost   \x1b[91mUSB start failed");
  } else if (st->ejected) {
    line(4, "\x1b[96mHost   \x1b[93mejected, safe to unplug");
  } else if (st->mounted) {
    line(4, "\x1b[96mHost   \x1b[92mconnected");
  } else {
    line(4, "\x1b[96mHost   \x1b[97mwaiting for host");
  }
  human_size(st->bytes_read, rd, sizeof(rd));
  human_size(st->bytes_written, wr, sizeof(wr));
  line(5, "\x1b[96mRead   \x1b[97m%-10s\x1b[96mWrite  \x1b[97m%s", rd, wr);
  macjr_display_flush(s_tty);
}

static void on_monitor_closed(void *ctx) {
  (void)ctx;
  s_redraw = true;
}

/* Never returns: the card stays with the host until the next reboot.
 * This program itself keeps running from PSRAM after the card is gone. */
int main(int argc, char *argv[], macjr_tty_t *tty) {
  s_tty = tty;
  list_root();

  sdmmc_card_t *card = NULL;
  bool usb_ok = false;
  if (sdcard_open_raw(&card) == ESP_OK) {
    usbdev_msc_set_card(card);
    usb_ok = usbdev_start(USBDEV_MODE_MSC) == ESP_OK;
  } else {
    ESP_LOGE(TAG, "no SD card to share");
    card = NULL;
  }

  draw_static(card);
  usbdev_msc_status_t last = {0}, now;
  draw_status(&last, usb_ok);
  monitor_set_redraw_cb(on_monitor_closed, NULL);
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
    if (monitor_is_open()) {
      continue; /* the monitor owns the screen */
    }
    usbdev_msc_status(&now);
    if (s_redraw || now.mounted != last.mounted || now.ejected != last.ejected ||
        now.bytes_read != last.bytes_read || now.bytes_written != last.bytes_written) {
      s_redraw = false;
      last = now;
      draw_status(&now, usb_ok);
    }
  }
}
