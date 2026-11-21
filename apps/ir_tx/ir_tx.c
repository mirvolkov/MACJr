/*
 * IR_TX.ELF -- IR sender: lists the codes IR_RX saved (IR/CODES.TXT) by
 * number; П→x / x→П move, В↑ sends the selected one. ВП opens a window to
 * type a protocol, address and command and send them. Cx ends it.
 *
 *   ════════════════[IR sender]═════════════
 *     1  NEC      0x00/0x45                ▲
 *     2  NEC      0x00/0x46                ■
 *    12  raw      68 durations             ▼
 *
 *   Sent  #1 NEC 0x00/0x45
 *    В↑ send   ВП manual   Cx exit
 *
 * The manual window (numbers in decimal, as the keys have no A-F):
 *
 *       ╔══════════[Manual]══════════╗
 *       ║ Protocol  NEC              ║
 *       ║ Address   0_         0x00  ║
 *       ║ Command   69         0x45  ║
 *       ║                            ║
 *       ║ Sent                       ║
 *       ╚════════════════════════════╝
 *
 * File format: see IR_RX. A number saved twice: its last line counts.
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
#include "ir_tx.h"
#include "keyboard.h"
#include "macjr.h"
#include "monitor.h"
#include "tty/list.h"
#include "tty/window.h"

#define IR_FILE "/sdcard/IR/CODES.TXT"

#define KEY_UP     KEY_MK_P_TO_X   /* П→x */
#define KEY_DOWN   KEY_MK_X_TO_P   /* x→П */
#define KEY_HOME   KEY_MK_HOME     /* ←ШГ: first item */
#define KEY_END    KEY_MK_END      /* ШГ→: last item */
#define KEY_OK     KEY_MK_ENTER    /* В↑ */
#define KEY_MANUAL KEY_MK_EE       /* ВП */
#define KEY_PROTO  KEY_MK_SP       /* С/П */
#define KEY_DEL    KEY_MK_STEP_BCK /* ←ШГ */

#define ROW_LIST   2
#define LIST_ROWS  12
#define ROW_STATUS 14
#define ROW_HINT   15
#define COLS       40
#define POLL_MS    100 /* how often a monitor redraw is checked */

#define MAX_CODES 100
#define RAW_MAX   128
#define FILE_MAX  (64 * 1024)

typedef struct {
  int num;
  ir_event_t ev;
  const char *raw; /* IR_PROTO_UNKNOWN: the durations, in the file text */
  unsigned raw_n;
} code_t;

static macjr_tty_t *s_tty;
static int s_kbd = -1;
static volatile bool s_redraw; /* set when the monitor closes */

static char *s_text; /* IR_FILE, read whole */
static code_t s_codes[MAX_CODES];
static int s_count;
static const char *s_load_err; /* why the list is empty */
static macjr_tty_list_t s_list;
static char s_status[48];

/* The manual window */
static const macjr_tty_window_t s_win = {4, 5, 32, 7};
static bool s_manual;
static int s_field; /* 0 address, 1 command */
static ir_proto_t s_mproto = IR_PROTO_NEC;
static unsigned s_mval[2];
static char s_mmsg[32];
static const char *s_mmsg_color;

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

/* ---- the file ---- */

static bool proto_of(const char *tok, size_t len, ir_proto_t *p) {
  static const struct {
    const char *name;
    ir_proto_t proto;
  } names[] = {{"NEC", IR_PROTO_NEC}, {"SAMSUNG", IR_PROTO_SAMSUNG}, {"SONY", IR_PROTO_SONY},
               {"RC5", IR_PROTO_RC5}, {"RAW", IR_PROTO_UNKNOWN}};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    if (strlen(names[i].name) == len && strncmp(names[i].name, tok, len) == 0) {
      *p = names[i].proto;
      return true;
    }
  }
  return false;
}

/* Parse the line at `s` (ends at '\n' or '\0') into `c`. */
static bool parse(const char *s, code_t *c) {
  char *end;
  memset(c, 0, sizeof(*c));
  c->num = (int)strtoul(s, &end, 10);
  if (end == s) {
    return false;
  }
  s = end;
  while (*s == ' ') {
    s++;
  }
  size_t len = strcspn(s, " \r\n");
  if (!proto_of(s, len, &c->ev.proto)) {
    return false;
  }
  s += len;
  if (c->ev.proto == IR_PROTO_UNKNOWN) {
    c->raw_n = strtoul(s, &end, 10);
    c->raw = end;
    return end != s && c->raw_n > 0 && c->raw_n <= RAW_MAX;
  }
  unsigned long v[4];
  for (int i = 0; i < 4; i++) {
    v[i] = strtoul(s, &end, 10);
    if (end == s) {
      return false;
    }
    s = end;
  }
  c->ev.addr = (uint16_t)v[0];
  c->ev.cmd = (uint8_t)v[1];
  c->ev.ext = (uint8_t)v[2];
  c->ev.edges = (uint16_t)v[3];
  return true;
}

static void load(void) {
  s_count = 0;
  s_load_err = NULL;
  free(s_text);
  s_text = NULL;
  struct stat st;
  if (stat(IR_FILE, &st) != 0) {
    s_load_err = "No codes yet: save some with IR_RX";
    return;
  }
  size_t size = st.st_size < FILE_MAX ? (size_t)st.st_size : FILE_MAX;
  s_text = malloc(size + 1);
  int fd = s_text ? open(IR_FILE, O_RDONLY) : -1;
  if (fd < 0) {
    s_load_err = "Can't read IR/CODES.TXT";
    return;
  }
  size_t got = 0;
  ssize_t n;
  while (got < size && (n = read(fd, s_text + got, size - got)) > 0) {
    got += n;
  }
  close(fd);
  s_text[got] = '\0';

  for (const char *p = s_text; *p;) {
    code_t c;
    if (parse(p, &c)) {
      int i = 0;
      while (i < s_count && s_codes[i].num < c.num) {
        i++;
      }
      if (i < s_count && s_codes[i].num == c.num) {
        s_codes[i] = c; /* saved again: the later line counts */
      } else if (s_count < MAX_CODES) {
        memmove(&s_codes[i + 1], &s_codes[i], (s_count - i) * sizeof(code_t));
        s_codes[i] = c;
        s_count++;
      }
    }
    p += strcspn(p, "\n");
    if (*p) {
      p++;
    }
  }
  if (s_count == 0) {
    s_load_err = "No codes in IR/CODES.TXT";
  }
}

/* ---- sending ---- */

static void describe(const code_t *c, char *buf, size_t len) {
  if (c->ev.proto == IR_PROTO_UNKNOWN) {
    snprintf(buf, len, "raw, %u durations", c->raw_n);
  } else {
    snprintf(buf, len, "%s 0x%02X/0x%02X", ir_proto_name(c->ev.proto), c->ev.addr, c->ev.cmd);
  }
}

static esp_err_t send(const code_t *c) {
  if (c->ev.proto != IR_PROTO_UNKNOWN) {
    return ir_tx_send(&c->ev);
  }
  uint32_t us[RAW_MAX];
  const char *s = c->raw;
  for (unsigned i = 0; i < c->raw_n; i++) {
    char *end;
    us[i] = strtoul(s, &end, 10);
    if (end == s) {
      return ESP_ERR_INVALID_SIZE; /* line cut short */
    }
    s = end;
  }
  return ir_tx_send_raw(us, c->raw_n, IR_TX_CARRIER_HZ);
}

static void send_selected(void) {
  if (s_list.sel < 0) {
    return;
  }
  const code_t *c = &s_codes[s_list.sel];
  esp_err_t err = send(c);
  char what[32];
  describe(c, what, sizeof(what));
  if (err == ESP_OK) {
    snprintf(s_status, sizeof(s_status), "#%d %s", c->num, what);
  } else {
    snprintf(s_status, sizeof(s_status), "\x1b[91mfailed: %s", esp_err_to_name(err));
  }
}

/* ---- the manual window ---- */

/* The largest address (field 0) or command (field 1) `p` carries. */
static unsigned field_max(ir_proto_t p, int field) {
  switch (p) {
  case IR_PROTO_NEC:
  case IR_PROTO_SAMSUNG:
    return field == 0 ? 0xFFFF : 0xFF;
  case IR_PROTO_SONY:
    return field == 0 ? 0xFF : 0x7F;
  default: /* RC-5 */
    return field == 0 ? 0x1F : 0x7F;
  }
}

static void manual_send(void) {
  code_t c = {0};
  c.ev.proto = s_mproto;
  c.ev.addr = (uint16_t)s_mval[0];
  c.ev.cmd = (uint8_t)s_mval[1];
  if (s_mproto == IR_PROTO_SONY) {
    c.ev.edges = s_mval[0] > 0x1F ? 32 : 26; /* 15 bit frame for an 8 bit address, else 12 bit */
  }
  esp_err_t err = send(&c);
  char what[32];
  describe(&c, what, sizeof(what));
  if (err == ESP_OK) {
    snprintf(s_mmsg, sizeof(s_mmsg), "Sent");
    s_mmsg_color = "92";
    snprintf(s_status, sizeof(s_status), "%s", what);
  } else {
    snprintf(s_mmsg, sizeof(s_mmsg), "Failed: %s", esp_err_to_name(err));
    s_mmsg_color = "91";
  }
}

static void draw_manual(void) {
  char buf[40];
  macjr_tty_window_draw(s_tty, &s_win, "Manual");
  snprintf(buf, sizeof(buf), "Protocol  %s", ir_proto_name(s_mproto));
  macjr_tty_window_text(s_tty, &s_win, 0, "97", false, buf);
  static const char *names[2] = {"Address", "Command"};
  for (int f = 0; f < 2; f++) {
    char val[8];
    snprintf(val, sizeof(val), "%u%s", s_mval[f], f == s_field ? "_" : "");
    snprintf(buf, sizeof(buf), "%-10s%-11s0x%02X", names[f], val, s_mval[f]);
    macjr_tty_window_text(s_tty, &s_win, 1 + f, f == s_field ? "93" : "97", false, buf);
  }
  if (s_mmsg[0]) {
    macjr_tty_window_text(s_tty, &s_win, 4, s_mmsg_color, false, s_mmsg);
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

static void manual_key(uint8_t key) {
  unsigned *v = &s_mval[s_field];
  int d = digit_of(key);
  s_mmsg[0] = '\0';
  if (d >= 0) {
    if (*v * 10 + d <= field_max(s_mproto, s_field)) {
      *v = *v * 10 + d;
    }
  } else if (key == KEY_DEL) {
    *v /= 10;
  } else if (key == KEY_UP || key == KEY_DOWN) {
    s_field = !s_field;
  } else if (key == KEY_PROTO) {
    s_mproto = s_mproto == IR_PROTO_RC5 ? IR_PROTO_NEC : (ir_proto_t)(s_mproto + 1);
    for (int f = 0; f < 2; f++) {
      unsigned max = field_max(s_mproto, f);
      if (s_mval[f] > max) {
        s_mval[f] = max;
      }
    }
  } else if (key == KEY_OK) {
    if (s_field == 0) {
      s_field = 1;
    } else {
      manual_send();
    }
  } else if (key == KEY_ESC) {
    s_manual = false;
  }
}

/* ---- the screen ---- */

static void code_item(int index, char *text, size_t len, void *ctx) {
  const code_t *c = &s_codes[index];
  if (c->ev.proto == IR_PROTO_UNKNOWN) {
    snprintf(text, len, "%3d  \x1b[90mraw      \x1b[97m%u durations", c->num, c->raw_n);
  } else {
    snprintf(text, len, "%3d  \x1b[96m%-8s \x1b[97m0x%02X/0x%02X", c->num, ir_proto_name(c->ev.proto),
             c->ev.addr, c->ev.cmd);
  }
}

static void draw_all(void) {
  if (monitor_is_open()) {
    return; /* redrawn when it closes */
  }
  out("\x1b[?25l\x1b[0m\x1b[2J");
  macjr_tty_header(s_tty, "IR sender");
  macjr_tty_list_draw(s_tty, &s_list);
  if (s_count == 0 && s_load_err) {
    line(ROW_LIST, "\x1b[90m%s", s_load_err);
  }
  line(ROW_STATUS, "\x1b[0;96mSent  \x1b[97m%s", s_status);
  if (s_manual) {
    draw_manual();
    line(ROW_HINT, "\x1b[0;90m 0-9 \x98\x83" "del \x91/\x8F proto \x82^ ok Cx back"); /* ШГ С/П В↑ */
  } else {
    line(ROW_HINT, "\x1b[0;90m \x82^ send   \x82\x8F manual   Cx exit"); /* В↑ ВП */
  }
  out("\x1b[0m");
  macjr_display_flush(s_tty);
}

/* Handle a key; false when the program should end. */
static bool on_key(uint8_t key) {
  if (s_manual) {
    manual_key(key);
    return true;
  }
  if (key == KEY_UP) {
    macjr_tty_list_move(&s_list, -1);
  } else if (key == KEY_DOWN) {
    macjr_tty_list_move(&s_list, 1);
  } else if (key == KEY_HOME) {
    macjr_tty_list_move(&s_list, -s_list.count);
  } else if (key == KEY_END) {
    macjr_tty_list_move(&s_list, s_list.count);
  } else if (key == KEY_OK) {
    send_selected();
  } else if (key == KEY_MANUAL) {
    s_manual = true;
    s_field = 0;
    s_mmsg[0] = '\0';
  } else if (key == KEY_ESC) {
    return false;
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
  s_manual = false;
  s_text = NULL;
  macjr_tty_list_init(&s_list, ROW_LIST, 1, COLS, LIST_ROWS, code_item, NULL);
  load();
  macjr_tty_list_set_count(&s_list, s_count);
  monitor_set_redraw_cb(on_monitor_closed, NULL);

  s_kbd = open(KBD_DEV_PATH, O_RDONLY);
  draw_all();
  for (;;) {
    int key = -1;
    if (s_kbd >= 0) {
      fd_set keys;
      FD_ZERO(&keys);
      FD_SET(s_kbd, &keys);
      struct timeval timeout = {.tv_sec = 0, .tv_usec = POLL_MS * 1000};
      uint8_t k;
      if (select(s_kbd + 1, &keys, NULL, NULL, &timeout) > 0 && read(s_kbd, &k, 1) == 1) {
        key = k;
      }
    } else {
      sys_delay(POLL_MS);
    }
    if (monitor_is_open()) {
      continue; /* the monitor owns the screen */
    }
    if (key >= 0 && !KBD_IS_CHORD(key)) {
      if (!on_key((uint8_t)key)) {
        break;
      }
      draw_all();
    } else if (s_redraw) {
      s_redraw = false;
      draw_all();
    }
  }
  if (s_kbd >= 0) {
    close(s_kbd);
  }
  free(s_text);
  return 0;
}
