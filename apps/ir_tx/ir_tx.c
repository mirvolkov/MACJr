/*
 * IR_TX.ELF -- IR sender: lists the codes IR_RX saved in the db (keys
 * "ir.<number>") by number; П→x / x→П move, В↑ sends the selected one. ВП opens a window to
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
 * Format: see IR_RX. Codes saved before they moved to the db, in the
 * file OLD_FILE, are imported once at start (a number the db has already
 * keeps its code), and the file goes when all of it is in.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"
#include "esp_err.h"
#include "ir_rx.h"
#include "ir_tx.h"
#include "keyboard.h"
#include "macjr.h"
#include "monitor.h"
#include "tty/list.h"
#include "tty/window.h"

#define IR_KEY   "ir." /* + the number: the db key of a saved code */
#define OLD_DIR  "/sdcard/IR"
#define OLD_FILE OLD_DIR "/CODES.TXT"

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
#define NUM_MAX   999
#define FILE_MAX  (64 * 1024)

typedef struct {
  int num;
  ir_event_t ev;
  char *text;      /* the db value, owned */
  const char *raw; /* IR_PROTO_UNKNOWN: the durations, in `text` */
  unsigned raw_n;
} code_t;

static macjr_tty_t *s_tty;
static int s_kbd = -1;

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

/* Parse a code, "<PROTO> ..." as IR_RX saves it (ends at '\n' or '\0'),
 * into `c`; for a raw code c->raw points into `s`. */
static bool parse_value(const char *s, code_t *c) {
  char *end;
  memset(c, 0, sizeof(*c));
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

/* The number in a db key IR_KEY "<num>", or -1. */
static int num_of(const char *key) {
  const char *digits = key + strlen(IR_KEY);
  char *end;
  long num = strtol(digits, &end, 10);
  return end != digits && *end == '\0' && num >= 0 && num <= NUM_MAX ? (int)num : -1;
}

static void free_codes(void) {
  for (int i = 0; i < s_count; i++) {
    free(s_codes[i].text);
  }
  s_count = 0;
}

/* db_list(): a saved code into s_codes, kept in order of number. */
static bool add_code(const char *key, const char *value, void *ctx) {
  (void)ctx;
  int num = num_of(key);
  if (num < 0) {
    return true;
  }
  char *text = strdup(value);
  code_t c;
  if (!text || !parse_value(text, &c)) {
    free(text);
    return text != NULL;
  }
  if (s_count == MAX_CODES) {
    free(text);
    return false;
  }
  c.num = num;
  c.text = text;
  int i = 0;
  while (i < s_count && s_codes[i].num < num) {
    i++;
  }
  memmove(&s_codes[i + 1], &s_codes[i], (s_count - i) * sizeof(code_t));
  s_codes[i] = c;
  s_count++;
  return true;
}

/* db_list(): mark the numbers the db has. */
static bool mark_num(const char *key, const char *value, void *ctx) {
  (void)value;
  int num = num_of(key);
  if (num >= 0) {
    ((bool *)ctx)[num] = true;
  }
  return true;
}

/* Move the codes of OLD_FILE (lines "<number> <code>") into the db. A
 * number the db had before keeps its code; within the file the last line
 * counts. The file and its directory go once every code is in. */
static void import_old(void) {
  struct stat st;
  if (stat(OLD_FILE, &st) != 0) {
    return;
  }
  size_t size = st.st_size < FILE_MAX ? (size_t)st.st_size : FILE_MAX;
  char *text = malloc(size + 1);
  bool *had = calloc(NUM_MAX + 1, sizeof(bool));
  int fd = text && had ? open(OLD_FILE, O_RDONLY) : -1;
  if (fd < 0) {
    free(text);
    free(had);
    return;
  }
  size_t got = 0;
  ssize_t n;
  while (got < size && (n = read(fd, text + got, size - got)) > 0) {
    got += n;
  }
  close(fd);
  text[got] = '\0';
  db_list(IR_KEY, mark_num, had);

  int moved = 0;
  bool all_in = true;
  for (char *p = text; *p;) {
    char *eol = p + strcspn(p, "\r\n");
    char keep = *eol;
    *eol = '\0';
    char *end;
    long num = strtol(p, &end, 10);
    code_t c;
    if (end != p && num >= 0 && num <= NUM_MAX && parse_value(end, &c) && !had[num]) {
      char key[16];
      snprintf(key, sizeof(key), IR_KEY "%ld", num);
      while (*end == ' ') {
        end++;
      }
      esp_err_t err = db_update(key, end);
      if (err == ESP_ERR_NOT_FOUND) {
        err = db_create(key, end);
      }
      if (err == ESP_OK) {
        moved++;
      } else {
        all_in = false;
      }
    }
    *eol = keep;
    p = eol + strspn(eol, "\r\n");
  }
  free(text);
  free(had);
  if (all_in) {
    unlink(OLD_FILE);
    unlink(OLD_DIR "/._CODES.TXT"); /* macOS metadata, if a Mac wrote it */
    rmdir(OLD_DIR); /* only if nothing else is in it */
    snprintf(s_status, sizeof(s_status), "\x1b[92m%d codes moved to the db", moved);
  } else {
    snprintf(s_status, sizeof(s_status), "\x1b[91mIR/CODES.TXT: not all moved");
  }
}

static void load(void) {
  free_codes();
  s_load_err = NULL;
  import_old();
  if (db_list(IR_KEY, add_code, NULL) < 0) {
    s_load_err = "Can't read the db";
  } else if (s_count == 0) {
    s_load_err = "No codes yet: save some with IR_RX";
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

int main(int argc, char *argv[], macjr_tty_t *tty) {
  s_tty = tty;
  s_status[0] = '\0';
  s_manual = false;
  macjr_tty_list_init(&s_list, ROW_LIST, 1, COLS, LIST_ROWS, code_item, NULL);
  load();
  macjr_tty_list_set_count(&s_list, s_count);

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
      continue; /* the keys are the monitor's */
    }
    if (key >= 0 && !KBD_IS_CHORD(key)) {
      if (!on_key((uint8_t)key)) {
        break;
      }
      draw_all();
    }
  }
  if (s_kbd >= 0) {
    close(s_kbd);
  }
  free_codes();
  return 0;
}
