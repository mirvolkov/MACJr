/*
 * WIFI.ELF -- connect to a Wi-Fi network. Lists the networks around, strongest
 * first; П→x / x→П move through the list, В↑ asks for the password of the
 * selected one, В↑ again connects. A network that connects is saved in
 * the db (NET_DB_KEY_SSID / NET_DB_KEY_PASS), and its password is filled
 * in the next time it is picked. Passwords are digits only for now.
 *
 *   ════════════════ Wi-Fi ═════════════════
 *   home                             *****░
 *   office                           ***··█
 *   cafe                             **···█
 *   ...                                   ░
 *    В↑ join   С/П scan   Cx exit
 *
 * The password window opens over the list:
 *
 *     ╔══════════════ home ══════════════╗
 *     ║ Password                         ║
 *     ║ 12345678_                        ║
 *     ║                                  ║
 *     ║ Wrong password                   ║
 *     ╚══════════════════════════════════╝
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "db.h"
#include "esp_log.h"
#include "keyboard.h"
#include "macjr.h"
#include "monitor.h"
#include "net.h"
#include "tty/list.h"

#define KEY_UP   KEY_MK_P_TO_X   /* П→x */
#define KEY_DOWN KEY_MK_X_TO_P   /* x→П */
#define KEY_OK   KEY_MK_ENTER    /* В↑ */
#define KEY_SCAN KEY_MK_SP       /* С/П */
#define KEY_BACK KEY_ESC         /* Cx */
#define KEY_DEL  KEY_MK_STEP_BCK /* ←ШГ */

#define ROW_LIST   2
#define LIST_ROWS  13
#define ROW_HINT   15
#define COLS       40

/* Password window */
#define WIN_ROW  6
#define WIN_COL  3
#define WIN_W    36 /* frame included */
#define WIN_H    6
#define PASS_MIN 8  /* WPA */

#define MAX_APS    32
#define CONNECT_MS 15000
#define POLL_MS    100 /* how often a monitor redraw is checked */

static const char *TAG = "wifi";

typedef enum {
  SCREEN_LIST,
  SCREEN_PASSWORD,
  SCREEN_DONE, /* connected: any key goes back to the list */
} screen_t;

static macjr_tty_t *s_tty;
static int s_kbd = -1;
static volatile bool s_redraw; /* set when the monitor closes */

static net_ap_t s_aps[MAX_APS];
static int s_count = -1; /* -1: not scanned yet / scan failed */
static macjr_tty_list_t s_list;

static screen_t s_screen;
static char s_pass[NET_PASS_MAX + 1];
static const char *s_msg; /* in the password window, with its color */
static char s_msg_buf[48];

static char s_saved_ssid[NET_SSID_MAX + 1];
static char s_saved_pass[NET_PASS_MAX + 1];

static void out(const char *s) {
  macjr_tty_write(s_tty, s, strlen(s));
}

/* Move to row r, column c (1-based) and print the formatted text. */
static void at(int r, int c, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void at(int r, int c, const char *fmt, ...) {
  char buf[128];
  int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", r, c);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  out(buf);
}

/* A network name as the font can show it: other bytes (UTF-8 and
 * control codes) become '?'. */
static void printable(const char *ssid, char *dst, size_t len) {
  size_t i = 0;
  for (; ssid[i] && i + 1 < len; i++) {
    unsigned char c = (unsigned char)ssid[i];
    dst[i] = c >= 0x20 && c < 0x7F ? (char)c : '?';
  }
  dst[i] = '\0';
}

static int stars(int8_t rssi) {
  return rssi >= -55 ? 5 : rssi >= -65 ? 4 : rssi >= -75 ? 3 : rssi >= -85 ? 2 : 1;
}

/* List item: the name, then the signal as 5 stars. */
static void ap_item(int index, char *text, size_t len, void *ctx) {
  const net_ap_t *ap = &s_aps[index];
  char name[NET_SSID_MAX + 1];
  printable(ap->ssid, name, sizeof(name));
  char bar[6];
  int n = stars(ap->rssi);
  for (int i = 0; i < 5; i++) {
    bar[i] = i < n ? '*' : '\xFA'; /* · */
  }
  bar[5] = '\0';
  snprintf(text, len, "%-33.32s%s", name, bar);
}

/* In place of an empty list: why it is empty. */
static void draw_empty(void) {
  if (s_count < 0) {
    at(ROW_LIST, 1, "\x1b[91mScan failed");
  } else if (s_count == 0) {
    at(ROW_LIST, 1, "\x1b[90mNo networks found");
  }
}

/* Frame row of the password window: corner `l`, `fill` with `title` in
 * the middle (if any), corner `right`. */
static void frame_row(int r, char l, char fill, char right, const char *title) {
  char buf[WIN_W + 1];
  memset(buf, fill, WIN_W);
  buf[0] = l;
  buf[WIN_W - 1] = right;
  buf[WIN_W] = '\0';
  if (title) {
    int len = (int)strlen(title);
    if (len > WIN_W - 6) {
      len = WIN_W - 6;
    }
    int start = (WIN_W - len - 2) / 2;
    buf[start] = ' ';
    memcpy(buf + start + 1, title, len);
    buf[start + 1 + len] = ' ';
  }
  at(r, WIN_COL, "\x1b[0;96;44m%s", buf);
}

/* Text row of the password window: frame, `text` (may hold colors), pad. */
static void text_row(int r, const char *color, const char *text) {
  int w = WIN_W - 4;
  at(r, WIN_COL, "\x1b[0;96;44m\xBA \x1b[%sm%-*.*s\x1b[96m \xBA", color, w, w, text);
}

static void draw_window(void) {
  char name[NET_SSID_MAX + 1];
  printable(s_aps[s_list.sel].ssid, name, sizeof(name));
  frame_row(WIN_ROW, '\xC9', '\xCD', '\xBB', name); /* ╔═╗ */
  text_row(WIN_ROW + 1, "97;44", s_aps[s_list.sel].open ? "Password (open network)" : "Password");
  /* the end of the password if it is longer than the row, then the cursor */
  int w = WIN_W - 5;
  size_t len = strlen(s_pass);
  const char *shown = len > (size_t)w ? s_pass + len - w : s_pass;
  char field[NET_PASS_MAX + 2];
  snprintf(field, sizeof(field), "%s%s", shown, s_screen == SCREEN_PASSWORD ? "_" : "");
  text_row(WIN_ROW + 2, "93;44", field);
  text_row(WIN_ROW + 3, "97;44", "");
  text_row(WIN_ROW + 4, s_msg ? s_msg : "97;44", s_msg ? s_msg_buf : "");
  frame_row(WIN_ROW + 5, '\xC8', '\xCD', '\xBC', NULL); /* ╚═╝ */
}

static void draw_all(void) {
  if (monitor_is_open()) {
    return; /* redrawn when it closes */
  }
  out("\x1b[?25l\x1b[0m\x1b[2J");
  macjr_tty_header(s_tty, "Wi-Fi");
  macjr_tty_list_draw(s_tty, &s_list);
  draw_empty();
  switch (s_screen) {
  case SCREEN_LIST:
    at(ROW_HINT, 1, "\x1b[0;90m \x82^ join   \x91/\x8F scan   Cx exit");
    break;
  case SCREEN_PASSWORD:
    draw_window();
    at(ROW_HINT, 1, "\x1b[0;90m 0-9 digits \x98\x83 del  \x82^ join  Cx back");
    break;
  case SCREEN_DONE:
    draw_window();
    at(ROW_HINT, 1, "\x1b[0;90m Any key: back to the list");
    break;
  }
  out("\x1b[0m");
  macjr_display_flush(s_tty);
}

/* Show `text` in the password window's message row, in `color` (SGR). */
static void message(const char *color, const char *text) {
  s_msg = color;
  snprintf(s_msg_buf, sizeof(s_msg_buf), "%s", text);
}

/* Wait up to `ms` for a key; the key, or -1. */
static int wait_key(unsigned ms) {
  if (s_kbd < 0) {
    sys_delay(ms);
    return -1;
  }
  fd_set keys;
  FD_ZERO(&keys);
  FD_SET(s_kbd, &keys);
  struct timeval timeout = {.tv_sec = 0, .tv_usec = ms * 1000};
  uint8_t key;
  if (select(s_kbd + 1, &keys, NULL, NULL, &timeout) > 0 && read(s_kbd, &key, 1) == 1) {
    return key;
  }
  return -1;
}

/* Drop the keys pressed while the program was busy (scanning,
 * connecting). */
static void drain_keys(void) {
  while (s_kbd >= 0 && wait_key(0) >= 0) {
  }
}


static void scan(void) {
  s_count = 1; /* nothing to explain under "Scanning..." */
  macjr_tty_list_set_count(&s_list, 0);
  draw_all();
  if (!monitor_is_open()) {
    at(ROW_LIST, 1, "\x1b[93mScanning...\x1b[0m");
    macjr_display_flush(s_tty);
  }
  s_count = net_wifi_scan(s_aps, MAX_APS);
  macjr_tty_list_set_count(&s_list, s_count < 0 ? 0 : s_count);
  s_list.sel = s_count > 0 ? 0 : -1;
  s_list.top = 0;
  drain_keys();
}

static void load_saved(void) {
  if (db_read(NET_DB_KEY_SSID, s_saved_ssid, sizeof(s_saved_ssid)) != ESP_OK ||
      db_read(NET_DB_KEY_PASS, s_saved_pass, sizeof(s_saved_pass)) != ESP_OK) {
    s_saved_ssid[0] = s_saved_pass[0] = '\0';
  }
}

static esp_err_t put(const char *key, const char *value) {
  esp_err_t err = db_update(key, value);
  return err == ESP_ERR_NOT_FOUND ? db_create(key, value) : err;
}

static void save(const char *ssid, const char *pass) {
  esp_err_t err = put(NET_DB_KEY_SSID, ssid);
  if (err == ESP_OK) {
    err = put(NET_DB_KEY_PASS, pass);
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "saving the network failed: %s", esp_err_to_name(err));
    return;
  }
  snprintf(s_saved_ssid, sizeof(s_saved_ssid), "%s", ssid);
  snprintf(s_saved_pass, sizeof(s_saved_pass), "%s", pass);
}

static void open_window(void) {
  const net_ap_t *ap = &s_aps[s_list.sel];
  bool saved = strcmp(ap->ssid, s_saved_ssid) == 0;
  snprintf(s_pass, sizeof(s_pass), "%s", saved ? s_saved_pass : "");
  s_msg = NULL;
  if (saved) {
    message("90;44", "Saved password");
  }
  s_screen = SCREEN_PASSWORD;
}

static void connect(void) {
  const net_ap_t *ap = &s_aps[s_list.sel];
  if (!ap->open && strlen(s_pass) < PASS_MIN) {
    message("91;44", "At least 8 digits");
    return;
  }
  message("93;44", "Connecting...");
  draw_all();
  esp_err_t err = net_wifi_connect(ap->ssid, s_pass, CONNECT_MS);
  drain_keys();
  if (err == ESP_OK) {
    char ip[16];
    net_wifi_status(NULL, ip);
    snprintf(s_msg_buf, sizeof(s_msg_buf), "Connected, %s", ip);
    s_msg = "92;44";
    save(ap->ssid, s_pass);
    s_screen = SCREEN_DONE;
  } else if (err == ESP_ERR_WIFI_PASSWORD) {
    message("91;44", "Wrong password");
  } else if (err == ESP_ERR_WIFI_SSID) {
    message("91;44", "Network not found");
  } else if (err == ESP_ERR_TIMEOUT) {
    message("91;44", "No answer");
  } else {
    message("91;44", "Failed to connect");
  }
}

static int digit_of(uint8_t code) {
  static const uint8_t digits[10] = {KEY_MK_0, KEY_MK_1, KEY_MK_2, KEY_MK_3, KEY_MK_4,
                                     KEY_MK_5, KEY_MK_6, KEY_MK_7, KEY_MK_8, KEY_MK_9};
  for (int i = 0; i < 10; i++) {
    if (digits[i] == code) {
      return i;
    }
  }
  return -1;
}

/* Handle a key; false when the program should end. */
static bool on_key(uint8_t key) {
  switch (s_screen) {
  case SCREEN_LIST:
    if (key == KEY_UP) {
      macjr_tty_list_move(&s_list, -1);
    } else if (key == KEY_DOWN) {
      macjr_tty_list_move(&s_list, 1);
    } else if (key == KEY_OK && s_list.sel >= 0) {
      open_window();
    } else if (key == KEY_SCAN) {
      scan();
    } else if (key == KEY_BACK) {
      return false;
    }
    break;
  case SCREEN_PASSWORD: {
    size_t len = strlen(s_pass);
    int d = digit_of(key);
    if (d >= 0 && len < NET_PASS_MAX) {
      s_pass[len] = (char)('0' + d);
      s_pass[len + 1] = '\0';
      s_msg = NULL;
    } else if (key == KEY_DEL && len > 0) {
      s_pass[len - 1] = '\0';
      s_msg = NULL;
    } else if (key == KEY_OK) {
      connect();
    } else if (key == KEY_BACK) {
      s_screen = SCREEN_LIST;
    }
    break;
  }
  case SCREEN_DONE:
    s_screen = SCREEN_LIST;
    break;
  }
  return true;
}

static void on_monitor_closed(void *ctx) {
  (void)ctx;
  s_redraw = true;
}

int main(int argc, char *argv[], macjr_tty_t *tty) {
  s_tty = tty;
  s_screen = SCREEN_LIST;
  macjr_tty_list_init(&s_list, ROW_LIST, 1, COLS, LIST_ROWS, ap_item, NULL);
  monitor_set_redraw_cb(on_monitor_closed, NULL);
  load_saved();

  s_kbd = open(KBD_DEV_PATH, O_RDONLY);
  scan();
  draw_all();
  for (;;) {
    int key = wait_key(POLL_MS);
    if (key >= 0 && !monitor_is_open()) {
      if (!on_key((uint8_t)key)) {
        break;
      }
      draw_all();
    }
    if (s_redraw) {
      s_redraw = false;
      draw_all();
    }
  }
  monitor_set_redraw_cb(NULL, NULL);
  if (s_kbd >= 0) {
    close(s_kbd);
    s_kbd = -1;
  }
  out("\x1b[0m");
  return 0;
}
