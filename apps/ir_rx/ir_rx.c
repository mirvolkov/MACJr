/*
 * IR_RX.ELF -- IR receiver: shows the last command received; ПП saves it
 * under a number typed in a window, for IR_TX to send. Cx ends it.
 *
 *   ═══════════════[IR receiver]════════════
 *
 *   Protocol  NEC
 *   Address   0x00
 *   Command   0x45
 *   Extra     0x00
 *   Edges     68
 *
 *   Saved     #12 NEC 0x00/0x45
 *
 *    ПП save   Cx exit
 *
 * The number window opens over the screen:
 *
 *          ╔═════[Save as]═════╗
 *          ║ NEC 0x00/0x45     ║
 *          ║ Number 12_        ║
 *          ╚═══════════════════╝
 *
 * Codes are appended to IR_FILE as text lines, one per code:
 *
 *   <number> <PROTO> <addr> <cmd> <ext> <edges>   (NEC SAMSUNG SONY RC5)
 *   <number> RAW <n> <us> <us> ...                (a protocol not decoded)
 *
 * Saving a number again adds a new line; the last one counts.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "ir_rx.h"
#include "keyboard.h"
#include "macjr.h"
#include "monitor.h"
#include "tty/window.h"

#define IR_DIR  "/sdcard/IR"
#define IR_FILE IR_DIR "/CODES.TXT"

#define KEY_SAVE KEY_MK_PP       /* ПП (159) */
#define KEY_OK   KEY_MK_ENTER    /* В↑ */
#define KEY_DEL  KEY_MK_STEP_BCK /* ←ШГ */

#define POLL_MS   20 /* select() timeout: how often IR is checked */
#define ROW_FIRST 3
#define ROW_SAVED 10
#define ROW_HINT  15
#define RAW_MAX   128
#define NUM_MAX   999
#define LINE_MAX  (RAW_MAX * 6 + 32)

typedef struct {
  ir_event_t ev;
  uint32_t raw[RAW_MAX]; /* the burst, if ev.proto is IR_PROTO_UNKNOWN */
  size_t raw_len;
} code_t;

static macjr_tty_t *s_tty;
static volatile bool s_redraw; /* set when the monitor closes */

static code_t s_last;   /* the last command received */
static bool s_have_last;
static code_t s_saving; /* what the number window saves */
static bool s_window;   /* the number window is open */
static char s_num[4];   /* digits typed in it */
static char s_status[48]; /* the "Saved" row, with its colors */
static const macjr_tty_window_t s_win = {6, 9, 24, 4};

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

/* "NEC 0x00/0x45" or "raw, 68 durations" */
static void describe(const code_t *c, char *buf, size_t len) {
  if (c->ev.proto == IR_PROTO_UNKNOWN) {
    snprintf(buf, len, "raw, %u durations", (unsigned)c->raw_len);
  } else {
    snprintf(buf, len, "%s 0x%02X/0x%02X", ir_proto_name(c->ev.proto), c->ev.addr, c->ev.cmd);
  }
}

static void draw_last(void) {
  if (!s_have_last) {
    line(ROW_FIRST, "\x1b[90mWaiting for a remote...");
    return;
  }
  const ir_event_t *ev = &s_last.ev;
  line(ROW_FIRST, "\x1b[96mProtocol  \x1b[97m%s%s", ir_proto_name(ev->proto), ev->repeat ? " (held)" : "");
  if (ev->proto == IR_PROTO_UNKNOWN) {
    line(ROW_FIRST + 1, "\x1b[96mAddress   \x1b[90m-");
    line(ROW_FIRST + 2, "\x1b[96mCommand   \x1b[90m-");
    line(ROW_FIRST + 3, "\x1b[96mExtra     \x1b[90m-");
  } else {
    line(ROW_FIRST + 1, "\x1b[96mAddress   \x1b[97m0x%02X", ev->addr);
    line(ROW_FIRST + 2, "\x1b[96mCommand   \x1b[97m0x%02X", ev->cmd);
    line(ROW_FIRST + 3, "\x1b[96m%-10s\x1b[97m0x%02X", ev->proto == IR_PROTO_RC5 ? "Toggle" : "Extra", ev->ext);
  }
  line(ROW_FIRST + 4, "\x1b[96mEdges     \x1b[97m%u", ev->edges);
}

static void draw_status(void) {
  line(ROW_SAVED, "\x1b[96mSaved     \x1b[97m%s", s_status);
}

static void draw_window(void) {
  char what[40];
  describe(&s_saving, what, sizeof(what));
  macjr_tty_window_draw(s_tty, &s_win, "Save as");
  macjr_tty_window_text(s_tty, &s_win, 0, "97", false, what);
  char field[24];
  snprintf(field, sizeof(field), "Number %s_", s_num);
  macjr_tty_window_text(s_tty, &s_win, 1, "93", false, field);
}

static void draw_hint(void) {
  if (s_window) {
    line(ROW_HINT, "\x1b[0;90m 0-9  \x98\x83 del  \x82^ save  Cx back"); /* ШГ, В↑ */
  } else {
    line(ROW_HINT, "\x1b[0;90m \x8F\x8F save   Cx exit"); /* ПП */
  }
}

static void draw_all(void) {
  out("\x1b[?25l\x1b[0m\x1b[2J");
  macjr_tty_header(s_tty, "IR receiver");
  draw_last();
  draw_status();
  draw_hint();
  if (s_window) {
    draw_window();
  }
  out("\x1b[0m");
  macjr_display_flush(s_tty);
}

static void received(const ir_event_t *ev) {
  s_last.ev = *ev;
  s_have_last = true;
  if (ev->proto == IR_PROTO_UNKNOWN) {
    s_last.raw_len = ir_rx_last_burst(s_last.raw, RAW_MAX);
    if (s_last.raw_len > RAW_MAX) {
      s_last.raw_len = RAW_MAX;
    }
  }
}

static const char *proto_token(ir_proto_t p) {
  switch (p) {
  case IR_PROTO_NEC:     return "NEC";
  case IR_PROTO_SAMSUNG: return "SAMSUNG";
  case IR_PROTO_SONY:    return "SONY";
  case IR_PROTO_RC5:     return "RC5";
  default:               return "RAW";
  }
}

/* Append `c` as number `num` to IR_FILE. */
static bool append(int num, const code_t *c) {
  static char buf[LINE_MAX];
  int n;
  if (c->ev.proto == IR_PROTO_UNKNOWN) {
    n = snprintf(buf, sizeof(buf), "%d RAW %u", num, (unsigned)c->raw_len);
    for (size_t i = 0; i < c->raw_len && n < (int)sizeof(buf) - 8; i++) {
      n += snprintf(buf + n, sizeof(buf) - n, " %u", (unsigned)c->raw[i]);
    }
  } else {
    n = snprintf(buf, sizeof(buf), "%d %s %u %u %u %u", num, proto_token(c->ev.proto), c->ev.addr, c->ev.cmd,
                 c->ev.ext, c->ev.edges);
  }
  buf[n++] = '\n';
  mkdir(IR_DIR, 0777); /* fails if it exists: fine */
  int fd = open(IR_FILE, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd < 0) {
    return false;
  }
  bool ok = write(fd, buf, n) == n;
  close(fd);
  return ok;
}

static void open_window(void) {
  if (!s_have_last) {
    snprintf(s_status, sizeof(s_status), "\x1b[91mnothing received yet");
    return;
  }
  s_saving = s_last;
  s_saving.ev.repeat = false; /* the whole command, not an NEC repeat code */
  s_num[0] = '\0';
  s_window = true;
}

static void save(void) {
  if (s_num[0] == '\0') {
    return; /* no number yet */
  }
  int num = atoi(s_num);
  char what[40];
  describe(&s_saving, what, sizeof(what));
  if (append(num, &s_saving)) {
    snprintf(s_status, sizeof(s_status), "#%d %s", num, what);
  } else {
    snprintf(s_status, sizeof(s_status), "\x1b[91mcan't write IR/CODES.TXT");
  }
  s_window = false;
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
  if (!s_window) {
    if (key == KEY_ESC) {
      return false;
    }
    if (key == KEY_SAVE) {
      open_window();
    }
    return true;
  }
  size_t len = strlen(s_num);
  int d = digit_of(key);
  if (d >= 0 && len < sizeof(s_num) - 1) {
    s_num[len] = (char)('0' + d);
    s_num[len + 1] = '\0';
    if (atoi(s_num) > NUM_MAX) {
      s_num[len] = '\0';
    }
  } else if (key == KEY_DEL && len > 0) {
    s_num[len - 1] = '\0';
  } else if (key == KEY_OK) {
    save();
  } else if (key == KEY_ESC) {
    s_window = false;
  }
  return true;
}

static void on_monitor_closed(void *ctx) {
  (void)ctx;
  s_redraw = true;
}

int main(int argc, char *argv[], macjr_tty_t *tty) {
  s_tty = tty;
  s_status[0] = '\0';
  s_window = false;
  s_have_last = false;
  ir_event_t ev;
  while (ir_rx_read(&ev, 0) == ESP_OK) {
    /* drop what came while the menu was up */
  }

  int fd = open(KBD_DEV_PATH, O_RDONLY);
  monitor_set_redraw_cb(on_monitor_closed, NULL);
  draw_all();

  for (;;) {
    bool dirty = false, full = false;
    /* wait for a key, at most POLL_MS: IR is not a descriptor (yet) */
    fd_set keys;
    FD_ZERO(&keys);
    bool key_ready = false;
    if (fd >= 0) {
      FD_SET(fd, &keys);
      struct timeval timeout = {.tv_sec = 0, .tv_usec = POLL_MS * 1000};
      key_ready = select(fd + 1, &keys, NULL, NULL, &timeout) > 0 && FD_ISSET(fd, &keys);
    } else {
      sys_delay(POLL_MS);
    }
    if (ir_rx_read(&ev, 0) == ESP_OK) {
      received(&ev);
      dirty = true;
    }
    uint8_t key;
    if (key_ready && read(fd, &key, 1) == 1 && !monitor_is_open() && !KBD_IS_CHORD(key)) {
      bool was_open = s_window;
      if (!on_key(key)) {
        break;
      }
      full = was_open || s_window; /* the window opened, changed or closed */
      dirty = true;
    }
    if (monitor_is_open()) {
      continue; /* the monitor owns the screen */
    }
    if (s_redraw || full) {
      s_redraw = false;
      draw_all();
    } else if (dirty && !s_window) {
      draw_last();
      draw_status();
      macjr_display_flush(s_tty);
    }
  }
  if (fd >= 0) {
    close(fd);
  }
  return 0;
}
