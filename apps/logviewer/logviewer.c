/*
 * LOGVIEWER.ELF -- the system log (/dev/log, logdev.h) as a table, oldest at
 * the top, newest at the bottom, following new records as they come
 * while the last row is selected. В↑ opens the whole record, ←ШГ / ШГ→
 * go to the ends, Cx exits.
 *
 *   ═══════════════[System log]═══════════════
 *    │App     │Tag     │Message
 *   I│SI5351_C│si5351  │CLK0 7.055000 MHz  ▲
 *   I│SI5351_C│si5351  │outputs on         ■
 *   E│SI5351_C│si5351  │the Si5351 doesn'  ▼
 *    В↑ open  ←ШГ ШГ→ ends  Cx exit
 *
 * The first column is the level (E W I D), and the row has its color:
 * errors red, warnings yellow, info white, debug grey. Records carry no
 * time and their numbers aren't shown: the order is the log's.
 */
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "keyboard.h"
#include "logdev.h"
#include "macjr.h"
#include "monitor.h"
#include "tty/list.h"
#include "tty/window.h"

#define COLS      40
#define ROW_TITLE 2
#define ROW_LIST  3
#define LIST_ROWS 12
#define ROW_HINT  15
#define POLL_MS   500

/* Column widths, a │ between them; the message takes the rest of the 39
 * cells left of the scroll bar */
#define W_LEVEL 1
#define W_APP   8
#define W_TAG   8
#define W_MSG   (COLS - 1 - W_LEVEL - W_APP - W_TAG - 3)

#define SEP "\x1b[90m\xB3" /* │ in grey */

#define DETAIL_W    38
#define DETAIL_H    13
#define DETAIL_TEXT (DETAIL_W - 4) /* inside the frame, a space each side */

static macjr_tty_t *s_tty;
static macjr_tty_list_t s_list;
static log_rec_t *s_recs; /* LOG_RING records, a ring */
static int s_start, s_count;

static void out(const char *s) {
  macjr_tty_write(s_tty, s, strlen(s));
}

static const log_rec_t *rec_at(int i) {
  return &s_recs[(s_start + i) % LOG_RING];
}

static const char *level_fg(int level) {
  switch (level) {
  case LOG_ERROR: return "91";
  case LOG_WARN:  return "93";
  case LOG_DEBUG: return "90";
  default:        return "97";
  }
}

static const char *level_name(int level) {
  switch (level) {
  case LOG_ERROR: return "Error";
  case LOG_WARN:  return "Warning";
  case LOG_DEBUG: return "Debug";
  default:        return "Info";
  }
}

/* A copy of `src` the screen can show: control characters as dots. */
static void printable(char *dst, const char *src, size_t len) {
  size_t i = 0;
  for (; src[i] && i < len - 1; i++) {
    dst[i] = (unsigned char)src[i] < 0x20 || src[i] == 0x7F ? '.' : src[i];
  }
  dst[i] = '\0';
}

static void row_item(int index, char *text, size_t len, void *ctx) {
  (void)ctx;
  const log_rec_t *r = rec_at(index);
  char app[LOG_NAME_MAX], tag[LOG_NAME_MAX], body[W_MSG + 1];
  printable(app, r->app, sizeof(app));
  printable(tag, r->tag, sizeof(tag));
  printable(body, r->body, sizeof(body));
  const char *c = level_fg(r->level);
  snprintf(text, len, "\x1b[%sm%c" SEP "\x1b[%sm%-*.*s" SEP "\x1b[%sm%-*.*s" SEP "\x1b[%sm%s", c,
           log_level_char(r->level), c, W_APP, W_APP, app, c, W_TAG, W_TAG, tag, c, body);
}

static void draw_all(void) {
  char buf[96];
  out("\x1b[?25l\x1b[0m\x1b[2J");
  macjr_tty_header(s_tty, "System log");
  snprintf(buf, sizeof(buf), "\x1b[%d;1H %s\x1b[96m%-*s" SEP "\x1b[96m%-*s" SEP "\x1b[96mMessage", ROW_TITLE,
           SEP, W_APP, "App", W_TAG, "Tag");
  out(buf);
  snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[0;90m %s", ROW_HINT,
           s_count ? "\x82^ open  <-\x98\x83 \x98\x83-> ends  Cx exit" /* В↑ ←ШГ ШГ→ */
                   : "No records yet            Cx exit");
  out(buf);
  macjr_tty_list_draw(s_tty, &s_list);
  macjr_display_flush(s_tty);
}

/* Take what the log has for us; true if anything came. */
static bool take_new(int fd) {
  bool any = false;
  for (;;) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    struct timeval now = {0, 0};
    if (select(fd + 1, &rd, NULL, NULL, &now) <= 0 ||
        log_read(fd, &s_recs[(s_start + s_count) % LOG_RING]) != 1) {
      break;
    }
    any = true;
    if (s_count < LOG_RING) {
      s_count++;
    } else {
      s_start = (s_start + 1) % LOG_RING; /* the oldest goes */
    }
  }
  return any;
}

/* The next key that is ours (not a chord, not the monitor's). */
static uint8_t next_key(int kbd) {
  for (;;) {
    uint8_t key;
    if (read(kbd, &key, 1) == 1 && !KBD_IS_CHORD(key) && !monitor_is_open()) {
      return key;
    }
  }
}

/* The whole record in a window over the table, until a key. */
static void show_detail(const log_rec_t *r, int kbd) {
  macjr_tty_window_t win;
  char line[64], app[LOG_NAME_MAX], tag[LOG_NAME_MAX], body[LOG_BODY_MAX];
  macjr_tty_window_center(s_tty, &win, DETAIL_W, DETAIL_H);
  macjr_tty_window_draw(s_tty, &win, level_name(r->level));
  printable(app, r->app, sizeof(app));
  printable(tag, r->tag, sizeof(tag));
  snprintf(line, sizeof(line), "%s%s%s", app, tag[0] ? " / " : "", tag);
  macjr_tty_window_text(s_tty, &win, 0, level_fg(r->level), false, line);

  /* the text, wrapped at spaces where it can be */
  printable(body, r->body, sizeof(body));
  const char *p = body;
  for (int y = 2; y < DETAIL_H - 2 && *p; y++) {
    int n = (int)strlen(p);
    if (n > DETAIL_TEXT) {
      n = DETAIL_TEXT;
      for (int k = DETAIL_TEXT; k > DETAIL_TEXT / 2; k--) {
        if (p[k] == ' ') {
          n = k;
          break;
        }
      }
    }
    snprintf(line, sizeof(line), "%.*s", n, p);
    macjr_tty_window_text(s_tty, &win, y, "97", false, line);
    p += n;
    while (*p == ' ') {
      p++;
    }
  }
  if (r->flags & LOG_LOST) {
    macjr_tty_window_text(s_tty, &win, DETAIL_H - 3, "93", false, "(records were lost before it)");
  }
  macjr_display_flush(s_tty);
  next_key(kbd);
}

int main(int argc, char *argv[], macjr_tty_t *tty) {
  s_tty = tty;
  s_start = s_count = 0;
  /* in PSRAM: LOG_RING records are too many for internal RAM */
  s_recs = heap_caps_realloc(NULL, LOG_RING * sizeof(log_rec_t), MALLOC_CAP_SPIRAM);
  int fd = log_open(0);
  int kbd = open(KBD_DEV_PATH, O_RDONLY);
  if (!s_recs || fd < 0 || kbd < 0) {
    out("\x1b[2J\x1b[1;1H\x1b[91mNo system log\r\n\x1b[90m Cx exit");
    macjr_display_flush(tty);
    while (sys_getkey() != SYS_KEY_ESC) {
    }
    heap_caps_free(s_recs);
    if (fd >= 0) {
      close(fd);
    }
    if (kbd >= 0) {
      close(kbd);
    }
    return 1;
  }

  macjr_tty_list_init(&s_list, ROW_LIST, 1, COLS, LIST_ROWS, row_item, NULL);
  take_new(fd);
  macjr_tty_list_set_count(&s_list, s_count);
  macjr_tty_list_move(&s_list, s_count); /* the newest */
  draw_all();

  for (;;) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    FD_SET(kbd, &rd);
    struct timeval timeout = {.tv_sec = 0, .tv_usec = POLL_MS * 1000};
    int maxfd = fd > kbd ? fd : kbd;
    if (select(maxfd + 1, &rd, NULL, NULL, &timeout) <= 0) {
      continue;
    }
    if (FD_ISSET(fd, &rd)) {
      bool at_end = s_list.sel >= s_count - 1;
      if (take_new(fd)) {
        macjr_tty_list_set_count(&s_list, s_count);
        if (at_end) {
          macjr_tty_list_move(&s_list, s_count);
        }
        draw_all();
      }
    }
    uint8_t key;
    if (!FD_ISSET(kbd, &rd) || read(kbd, &key, 1) != 1 || KBD_IS_CHORD(key) || monitor_is_open()) {
      continue;
    }
    if (key == KEY_ESC) {
      break;
    }
    int delta = 0;
    switch (key) {
    case KEY_MK_P_TO_X: delta = -1; break;
    case KEY_MK_X_TO_P: delta = 1; break;
    case KEY_MK_HOME:   delta = -s_count; break;
    case KEY_MK_END:    delta = s_count; break;
    case KEY_MK_ENTER:
      if (s_list.sel >= 0) {
        show_detail(rec_at(s_list.sel), kbd);
        draw_all();
      }
      break;
    }
    if (delta && macjr_tty_list_move(&s_list, delta)) {
      macjr_tty_list_draw(s_tty, &s_list);
      macjr_display_flush(s_tty);
    }
  }
  close(fd);
  close(kbd);
  heap_caps_free(s_recs);
  return 0;
}
