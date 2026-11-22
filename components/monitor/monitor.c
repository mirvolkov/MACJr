/*
 * System monitor: a task that sits on /dev/kbrd waiting for MONITOR_KEY.
 * It is a console app with a tty of its own, a second console on the
 * display: MONITOR_KEY brings that tty to the front with a live status
 * page, the next one puts the program's tty back. The program goes on
 * writing to its tty meanwhile, in the background, so what it shows is up
 * to date when it comes back. The monitor only talks to its tty.
 *
 * CPU load comes from FreeRTOS run-time stats: the share of wall time each
 * core's idle task did NOT run since the previous refresh (both counters
 * are esp_timer microseconds, so 32-bit wrap-around cancels out in deltas).
 *
 * Every line fits the 40x15 text screen.
 */
#include "monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard.h"
#include "net.h"
#include "sdcard.h"
#include "tty/list.h"
#include "tty/window.h"
#include "usbdev.h"

#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#error "monitor needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS for CPU load"
#endif

#define REFRESH_MS 1000
#define POLL_MS 50       /* key polling while the monitor is shown */
#define TASK_STACK 6144 /* screenshots encode a PNG here */
#define TASK_PRIO 3

/* The window that shows a screenshot's outcome */
#define NOTICE_MS 1000
#define NOTICE_W  30 /* frame included */
#define NOTICE_H  5

static const char *TAG = "monitor";

static void take_screenshot(void);

static macjr_tty_t *s_tty;  /* the programs' tty: screenshots of the display */
static macjr_tty_t *s_mon;  /* the monitor's own */
static volatile bool s_open;
static volatile bool s_enabled = true; /* MONITOR_KEY handled */
static temperature_sensor_handle_t s_tsens;

typedef struct {
  uint32_t idle[portNUM_PROCESSORS];
  uint32_t now;
} cpu_sample_t;

static void cpu_sample(cpu_sample_t *s) {
  for (int c = 0; c < portNUM_PROCESSORS; c++) {
    s->idle[c] = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(c);
  }
  s->now = (uint32_t)esp_timer_get_time();
}

/* Busy percentage of core c between two samples. */
static int cpu_load(const cpu_sample_t *a, const cpu_sample_t *b, int c) {
  uint32_t wall = b->now - a->now;
  uint32_t idle = b->idle[c] - a->idle[c];
  if (wall == 0 || idle >= wall) {
    return 0;
  }
  return (int)(100 - (uint64_t)idle * 100 / wall);
}

static void out(const char *s) {
  macjr_tty_write(s_mon, s, strlen(s));
}

/* The page is built as a list of lines and shown through the scrollable
 * list widget (rows 2-14, scroll bar in the last column), so it can hold
 * more than fits on screen. */
#define MON_VIEW_ROW 2
#define MON_VIEW_H   13
#define MON_MAX_LINES 32
static char s_lines[MON_MAX_LINES][128];
static int s_nlines;
static macjr_tty_list_t s_list;

static void addline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void addline(const char *fmt, ...) {
  if (s_nlines >= MON_MAX_LINES) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_lines[s_nlines], sizeof(s_lines[0]), fmt, ap);
  va_end(ap);
  s_nlines++;
}

static void blank(void) {
  if (s_nlines < MON_MAX_LINES) {
    s_lines[s_nlines++][0] = '\0';
  }
}

/* Green under 50%, yellow under 80%, red above. */
static const char *load_color(int pct) {
  return pct < 50 ? "\x1b[92m" : (pct < 80 ? "\x1b[93m" : "\x1b[91m");
}

/* One core's meter, as in htop: "0[|||||       23%]", the bars in the
 * load color, the figure in the meter's right end. */
#define METER_W 15 /* cells between the brackets */
static int cpu_meter(char *out, size_t len, int core, int pct) {
  char num[8];
  int nl = snprintf(num, sizeof(num), "%d%%", pct);
  int fill = (pct * METER_W + 50) / 100;
  int n = snprintf(out, len, "\x1b[96m%d\x1b[97m[%s", core, load_color(pct));
  for (int i = 0; i < METER_W && n + 8 < (int)len; i++) {
    if (i == METER_W - nl) {
      n += snprintf(out + n, len - n, "\x1b[37m");
    }
    out[n++] = i >= METER_W - nl ? num[i - (METER_W - nl)] : i < fill ? '|' : ' ';
  }
  n += snprintf(out + n, len - n, "\x1b[97m]");
  return n;
}

/* All cores on one line. */
static void cpu_line(const cpu_sample_t *prev, const cpu_sample_t *cur) {
  char text[sizeof(s_lines[0])];
  int n = 0;
  for (int c = 0; c < portNUM_PROCESSORS && n < (int)sizeof(text) - 1; c++) {
    if (c) {
      text[n++] = ' ';
    }
    n += cpu_meter(text + n, sizeof(text) - n, c, cpu_load(prev, cur, c));
  }
  text[n] = '\0';
  addline("%s", text);
}

static void mem_line(const char *name, uint32_t caps) {
  size_t total = heap_caps_get_total_size(caps);
  size_t used = total - heap_caps_get_free_size(caps);
  addline("\x1b[97m%-6s %5u / %5u KB used", name, (unsigned)(used / 1024), (unsigned)(total / 1024));
}

/* Rebuild the page's lines from the current stats. */
static void build_lines(const cpu_sample_t *prev, const cpu_sample_t *cur) {
  s_nlines = 0;
  cpu_line(prev, cur);
  blank();
  addline("\x1b[96mMemory");
  mem_line("SRAM", MALLOC_CAP_INTERNAL);
#if CONFIG_SPIRAM
  if (esp_psram_is_initialized()) {
    mem_line("PSRAM", MALLOC_CAP_SPIRAM);
  } else
#endif
  {
    addline("\x1b[97mPSRAM  disabled");
  }
  blank();

  float celsius;
  if (s_tsens && temperature_sensor_get_celsius(s_tsens, &celsius) == ESP_OK) {
    addline("\x1b[96mTemp   \x1b[97m%.1f C", celsius);
  } else {
    addline("\x1b[96mTemp   \x1b[97mn/a");
  }
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  if (tm.tm_year >= 120) { /* year >= 2020: the clock has been set (NTP) */
    addline("\x1b[96mTime   \x1b[97m%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
  }
  blank();

  char ssid[NET_SSID_MAX + 1], ip[40];
  uint8_t mac[6];
  bool online = net_wifi_status(ssid, ip);
  net_wifi_mac(mac);
  if (online) {
    for (char *c = ssid; *c; c++) {
      if ((unsigned char)*c < 0x20 || (unsigned char)*c >= 0x7F) {
        *c = '?'; /* the font has no UTF-8 */
      }
    }
    addline("\x1b[96mWi-Fi  \x1b[97m%.31s", ssid);
  } else {
    addline("\x1b[96mWi-Fi  \x1b[97mnot connected");
  }
  addline("\x1b[96m  MAC  \x1b[97m%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  if (online) {
    addline("\x1b[96m  IP   \x1b[97m%s", ip);
  }
  blank();

  uint64_t total, free_bytes;
  if (sdcard_info(&total, &free_bytes) == ESP_OK) {
    addline("\x1b[96mSD     \x1b[97m%lu / %lu MB free", (unsigned long)(free_bytes >> 20),
            (unsigned long)(total >> 20));
  } else {
    addline("\x1b[96mSD     \x1b[97mnot mounted");
  }
}

static void mon_item(int index, char *text, size_t len, void *ctx) {
  (void)ctx;
  snprintf(text, len, "%s", index < s_nlines ? s_lines[index] : "");
}

/* The frame, as the navigator's: double lines in grey, the page's scroll
 * bar for the right edge.
 *
 *   ╔═════════[MacJr system monitor]═════════╗
 *   ║ ...                                    ▲
 *   ╚═[F+K close  P->x x->P scroll]══════════╝ */
#define MON_COLS     40
#define COLOR_FRAME  "\x1b[0;37m"
#define COLOR_TITLE  "\x1b[0;97;40m"
#define COLOR_HINT   "\x1b[0;90;40m"

/* A horizontal edge on `row`: corners l and r, ═ between, `label` in
 * brackets at column `col` (0-based; -1 centers it). */
static void edge(int row, char l, char r, const char *label, const char *color, int col) {
  char bars[MON_COLS + 1];
  int len = (int)strlen(label);
  memset(bars, '\xCD', MON_COLS); /* ═ */
  bars[0] = l;
  bars[MON_COLS - 1] = r;
  bars[MON_COLS] = '\0';
  if (col < 0) {
    col = (MON_COLS - (len + 2)) / 2;
  }
  char buf[160];
  snprintf(buf, sizeof(buf), "\x1b[%d;1H" COLOR_FRAME "%.*s[%s%s" COLOR_FRAME "]%s\x1b[0m", row, col, bars,
           color, label, bars + col + len + 2);
  out(buf);
}

static void draw_static(void) {
  out("\x1b[?25l\x1b[0m\x1b[2J");
  edge(1, '\xC9', '\xBB', "MacJr system monitor", COLOR_TITLE, -1); /* ╔ ╗ */
  for (int r = MON_VIEW_ROW; r < MON_VIEW_ROW + MON_VIEW_H; r++) {
    char buf[24];
    snprintf(buf, sizeof(buf), "\x1b[%d;1H" COLOR_FRAME "\xBA", r); /* ║ */
    out(buf);
  }
  edge(MON_VIEW_ROW + MON_VIEW_H, '\xC8', '\xBC', "F+K close  P->x x->P scroll", COLOR_HINT, 2); /* ╚ ╝ */
}

/* Rebuild the lines and redraw the scrollable page. */
static void draw_stats(const cpu_sample_t *prev, const cpu_sample_t *cur) {
  build_lines(prev, cur);
  macjr_tty_list_set_count(&s_list, s_nlines);
  macjr_tty_list_draw_view(s_mon, &s_list);
  macjr_display_flush(s_mon);
}

/* Show the monitor until MONITOR_KEY comes in again on fd. */
static void run_monitor(int fd) {
  s_open = true; /* keys are the monitor's now */
  fcntl(fd, F_SETFL, O_NONBLOCK);
  draw_static();
  macjr_tty_set_front(s_mon);
  macjr_tty_list_init(&s_list, MON_VIEW_ROW, 2, MON_COLS - 1, MON_VIEW_H, mon_item, NULL);
  cpu_sample_t prev, cur;
  cpu_sample(&prev);
  vTaskDelay(pdMS_TO_TICKS(100)); /* first CPU figures after 100 ms, not 1 s */

  TickType_t next = xTaskGetTickCount();
  for (;;) {
    if ((int32_t)(xTaskGetTickCount() - next) >= 0) { /* refresh due */
      cpu_sample(&cur);
      draw_stats(&prev, &cur);
      prev = cur;
      next = xTaskGetTickCount() + pdMS_TO_TICKS(REFRESH_MS);
    }
    uint8_t key;
    if (read(fd, &key, 1) == 1) {
      if (key == MONITOR_KEY) {
        break;
      } else if (key == SCREENSHOT_KEY) {
        take_screenshot();
      } else if (key == KEY_MK_P_TO_X || key == KEY_MK_X_TO_P || key == KEY_MK_HOME || key == KEY_MK_END) {
        int delta = key == KEY_MK_P_TO_X ? -1 : key == KEY_MK_X_TO_P ? 1 : key == KEY_MK_HOME ? -s_list.count : s_list.count;
        if (macjr_tty_list_scroll(&s_list, delta)) {
          macjr_tty_list_draw_view(s_mon, &s_list);
          macjr_display_flush(s_mon);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }

  fcntl(fd, F_SETFL, 0);
  macjr_tty_set_front(s_tty);
  s_open = false;
}

/* A centered window with a title in its top edge and a line of text, on
 * the monitor's tty over a copy of what was on screen: the program's
 * screen, or the monitor page (kept and put back after). The program
 * goes on in the background meanwhile, as under the monitor page. */
typedef struct {
  macjr_tty_screen_t *saved; /* the monitor page, when it was open */
  bool was_open;
  macjr_tty_window_t win;
} notice_t;

/* (Re)draw the window's contents. */
static void notice_set(notice_t *n, const char *title, const char *text, const char *fg) {
  macjr_tty_window_draw(s_mon, &n->win, title);
  macjr_tty_window_text(s_mon, &n->win, (NOTICE_H - 2) / 2, fg, true, text);
  macjr_display_flush(s_mon);
}

/* Put the window up; false if a screen can't be copied (nothing shown). */
static bool notice_open(notice_t *n, const char *title, const char *text, const char *fg) {
  n->was_open = s_open;
  if (n->was_open) {
    n->saved = macjr_tty_save_screen(s_mon); /* the page, put back after */
    if (!n->saved) {
      return false;
    }
  } else {
    macjr_tty_screen_t *shown = macjr_tty_save_screen(s_tty);
    if (!shown) {
      return false;
    }
    macjr_tty_restore_screen(s_mon, shown); /* the program's screen, under the window */
    n->saved = NULL;
    s_open = true;
  }
  macjr_tty_window_center(s_mon, &n->win, NOTICE_W, NOTICE_H);
  notice_set(n, title, text, fg);
  if (!n->was_open) {
    macjr_tty_set_front(s_mon);
  }
  return true;
}

/* Take the window down: the monitor page back, or the program's tty. */
static void notice_close(notice_t *n) {
  if (n->was_open) {
    macjr_tty_restore_screen(s_mon, n->saved);
    macjr_display_flush(s_mon);
  } else {
    macjr_tty_set_front(s_tty);
    s_open = false;
  }
}

/* Show the window for NOTICE_MS. */
static void notice(const char *title, const char *text, const char *fg) {
  notice_t n;
  if (notice_open(&n, title, text, fg)) {
    vTaskDelay(pdMS_TO_TICKS(NOTICE_MS));
    notice_close(&n);
  }
}

/* SCREENSHOT_KEY: grab the picture, then say so at once -- the window
 * isn't in the picture -- while it is written to the card, which takes a
 * moment; then the outcome in the same window. */
static void take_screenshot(void) {
  char msg[48];
  uint8_t *rgb = screenshot_grab(s_tty, msg, sizeof(msg));
  if (!rgb) {
    notice("Screenshot failed", msg, "93");
    return;
  }
  notice_t n;
  bool shown = notice_open(&n, "Taking screenshot", "Saving to the SD card...", "97");
  bool ok = screenshot_write(s_tty, rgb, msg, sizeof(msg));
  if (!shown) {
    notice(ok ? "Screenshot taken" : "Screenshot failed", msg, ok ? "97" : "93");
    return;
  }
  notice_set(&n, ok ? "Screenshot taken" : "Screenshot failed", msg, ok ? "97" : "93");
  vTaskDelay(pdMS_TO_TICKS(NOTICE_MS));
  notice_close(&n);
}

/* USB_KBD_KEY: switch the USB keyboard on or off, and say so. */
static void toggle_usb_kbd(void) {
  if (usb_kbd_active()) {
    usb_kbd_stop();
    notice("USB keyboard", "Off", "97");
    return;
  }
  esp_err_t err = usb_kbd_start();
  if (err == ESP_OK) {
    notice("USB keyboard", "On: plug in a keyboard", "97");
  } else if (err == ESP_ERR_INVALID_STATE) {
    notice("USB keyboard", "USB is in use", "93");
  } else {
    notice("USB keyboard", esp_err_to_name(err), "93");
  }
}

/* WIFI_KEY: toggle Wi-Fi through the firmware's callback, and say so. */
static monitor_wifi_cb_t s_wifi_cb;

static void toggle_wifi(void) {
  if (!s_wifi_cb) {
    return;
  }
  bool on = s_wifi_cb();
  notice("Wi-Fi", on ? "On: joining the network" : "Off", "97");
}

void monitor_set_wifi_cb(monitor_wifi_cb_t cb) {
  s_wifi_cb = cb;
}

void monitor_set_enabled(bool on) {
  s_enabled = on;
}

bool monitor_is_open(void) {
  return s_open;
}

static void monitor_task(void *arg) {
  (void)arg;
  int fd = open(KBD_DEV_PATH, O_RDONLY);
  if (fd < 0) {
    ESP_LOGE(TAG, "open %s failed: errno %d", KBD_DEV_PATH, errno);
    vTaskDelete(NULL);
    return;
  }
  for (;;) {
    uint8_t key;
    if (read(fd, &key, 1) != 1) { /* blocks */
      continue;
    }
    if (key == MONITOR_KEY && s_enabled) {
      run_monitor(fd);
    } else if (key == SCREENSHOT_KEY) {
      take_screenshot();
    } else if (key == USB_KBD_KEY) {
      toggle_usb_kbd();
    } else if (key == WIFI_KEY) {
      toggle_wifi();
    }
  }
}

esp_err_t monitor_start(macjr_tty_t *tty, macjr_tty_t *own) {
  if (!tty || !own || own == tty) {
    return ESP_ERR_INVALID_ARG;
  }
  s_tty = tty;
  s_mon = own;

  temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  if (temperature_sensor_install(&tcfg, &s_tsens) != ESP_OK || temperature_sensor_enable(s_tsens) != ESP_OK) {
    ESP_LOGW(TAG, "temperature sensor unavailable");
    s_tsens = NULL;
  }

  if (xTaskCreate(monitor_task, "monitor", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}
