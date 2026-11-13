/*
 * System monitor: a task that sits on /dev/kbrd waiting for MONITOR_KEY.
 * When it arrives, the current tty screen is saved and replaced with a live
 * status page; the next MONITOR_KEY restores the saved screen.
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
#define TASK_STACK 6144 /* the redraw callback may decode a PNG */
#define TASK_PRIO 3
#define BAR_W 20

/* The window that shows a screenshot's outcome */
#define NOTICE_MS 1000
#define NOTICE_W  30 /* frame included */
#define NOTICE_H  5

/* CP866 glyphs from the built-in font. */
#define CH_FULL "\xDB"
#define CH_SHADE "\xB0"

static const char *TAG = "monitor";

static void take_screenshot(void);

static macjr_tty_t *s_tty;
static volatile bool s_open;
static volatile bool s_enabled = true; /* MONITOR_KEY handled */
static monitor_redraw_cb_t s_redraw_cb;
static void *s_redraw_ctx;
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

static void cpu_line(int core, int pct) {
  char bar[BAR_W * sizeof(CH_FULL) + 1];
  int fill = (pct * BAR_W + 50) / 100;
  bar[0] = '\0';
  for (int i = 0; i < BAR_W; i++) {
    strcat(bar, i < fill ? CH_FULL : CH_SHADE);
  }
  const char *color = pct < 50 ? "\x1b[92m" : (pct < 80 ? "\x1b[93m" : "\x1b[91m");
  addline("\x1b[97mCPU%d %s%s\x1b[97m %3d%%", core, color, bar, pct);
}

static void mem_line(const char *name, uint32_t caps) {
  size_t total = heap_caps_get_total_size(caps);
  size_t used = total - heap_caps_get_free_size(caps);
  addline("\x1b[97m%-6s %5u / %5u KB used", name, (unsigned)(used / 1024), (unsigned)(total / 1024));
}

/* Rebuild the page's lines from the current stats. */
static void build_lines(const cpu_sample_t *prev, const cpu_sample_t *cur) {
  s_nlines = 0;
  for (int c = 0; c < portNUM_PROCESSORS; c++) {
    cpu_line(c, cpu_load(prev, cur, c));
  }
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

static void draw_static(void) {
  out("\x1b[?25l\x1b[0m\x1b[2J");
  macjr_tty_header(s_tty, "MacJr system monitor");
  line(15, "\x1b[90m F+K close   P->x / x->P scroll");
}

/* Rebuild the lines and redraw the scrollable page. */
static void draw_stats(const cpu_sample_t *prev, const cpu_sample_t *cur) {
  build_lines(prev, cur);
  macjr_tty_list_set_count(&s_list, s_nlines);
  macjr_tty_list_draw(s_tty, &s_list);
  macjr_display_flush(s_tty);
}

/* Show the monitor until MONITOR_KEY comes in again on fd. */
static void run_monitor(int fd) {
  s_open = true; /* apps stop drawing before the screen is saved */
  macjr_tty_screen_t *saved = macjr_tty_save_screen(s_tty);
  if (!saved) {
    ESP_LOGE(TAG, "no memory to save the screen");
    s_open = false;
    return;
  }

  fcntl(fd, F_SETFL, O_NONBLOCK);
  draw_static();
  macjr_tty_list_init(&s_list, MON_VIEW_ROW, 1, 40, MON_VIEW_H, mon_item, NULL);
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
      } else if (key == KEY_MK_P_TO_X || key == KEY_MK_X_TO_P) {
        if (macjr_tty_list_move(&s_list, key == KEY_MK_P_TO_X ? -1 : 1)) {
          macjr_tty_list_draw(s_tty, &s_list);
          macjr_display_flush(s_tty);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }

  fcntl(fd, F_SETFL, 0);
  macjr_tty_restore_screen(s_tty, saved);
  macjr_display_flush(s_tty);
  s_open = false;
  if (s_redraw_cb) {
    s_redraw_cb(s_redraw_ctx); /* repaint what the app drew outside the tty */
  }
}

/* Show a centered window with `title` in its top edge and `text` inside
 * for NOTICE_MS, then put the screen back. Programs see the monitor open
 * meanwhile, so they don't draw over it; inside the monitor page it just
 * covers that page for a moment. */
static void notice(const char *title, const char *text, const char *fg) {
  bool was_open = s_open;
  s_open = true;
  macjr_tty_screen_t *saved = macjr_tty_save_screen(s_tty);
  if (!saved) {
    s_open = was_open;
    return;
  }
  macjr_tty_window_t win;
  macjr_tty_window_center(s_tty, &win, NOTICE_W, NOTICE_H);
  macjr_tty_window_draw(s_tty, &win, title);
  macjr_tty_window_text(s_tty, &win, (NOTICE_H - 2) / 2, fg, true, text);
  macjr_display_flush(s_tty);

  vTaskDelay(pdMS_TO_TICKS(NOTICE_MS));

  macjr_tty_restore_screen(s_tty, saved);
  macjr_display_flush(s_tty);
  if (!was_open) {
    s_open = false;
    if (s_redraw_cb) {
      s_redraw_cb(s_redraw_ctx); /* repaint what the app drew outside the tty */
    }
  }
}

/* SCREENSHOT_KEY: save the picture first, then say how it went. */
static void take_screenshot(void) {
  char msg[48];
  if (screenshot_save(s_tty, msg, sizeof(msg))) {
    notice("Screenshot taken", msg, "97");
  } else {
    notice("Screenshot failed", msg, "93");
  }
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

void monitor_set_redraw_cb(monitor_redraw_cb_t cb, void *ctx) {
  s_redraw_ctx = ctx;
  s_redraw_cb = cb;
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

esp_err_t monitor_start(macjr_tty_t *tty) {
  if (!tty) {
    return ESP_ERR_INVALID_ARG;
  }
  s_tty = tty;

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
