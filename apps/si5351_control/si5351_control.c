/*
 * SI5351_CONTROL.ELF -- the Si5351 as a 3-channel clock generator
 * (components/hal/sdr, generator mode). Each output CLK0..CLK2 gets its
 * own frequency; С/П enables or disables the chip's outputs as a whole.
 *
 *   ═══════════[Si5351 generator]═══════════
 *
 *   Outputs   ON                   (green ON / red OFF)
 *
 *   > CLK0      7.055000 MHz       (selected channel: yellow)
 *     CLK1    off
 *     CLK2     10.000000 MHz
 *
 *   Input    14.074_   MHz
 *   Out of range: 0.008-100 MHz    (red, after a bad В↑)
 *
 *   0.008 - 100 MHz, 0 turns off
 *    0-9 . MHz  ШГ del  В^ set
 *    П>x x>П chan  С/П on/off  Cx exit
 *
 * The chip keeps generating after the program ends. The settings are kept
 * in the config store (components/utils/db) as gen_f0..gen_f2=<Hz> and
 * gen_on=0|1 and restored at the next start.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "db.h"
#include "esp_log.h"
#include "keyboard.h"
#include "macjr.h"
#include "monitor.h"
#include "sdr.h"

#define INT_DIGITS 3  /* MHz part: up to 100 */
#define FRAC_DIGITS 6 /* down to 1 Hz */
#define KEY_SET    KEY_MK_ENTER    /* В↑ */
#define KEY_DEL    KEY_MK_STEP_BCK /* ←ШГ */
#define KEY_UP     KEY_MK_P_TO_X   /* П→x */
#define KEY_DOWN   KEY_MK_X_TO_P   /* x→П */
#define KEY_ONOFF  KEY_MK_SP       /* С/П */
#define KEY_BACK   KEY_ESC         /* Cx: drop what was typed, or exit */
#define CH         SDR_GEN_CHANNELS
#define POLL_MS    100 /* how often a monitor redraw is checked */

#define ROW_OUT    3
#define ROW_CH     5 /* CLK0; CLK1, CLK2 below */
#define ROW_INPUT  9
#define ROW_MSG    10
#define ROW_RANGE  13
#define ROW_HINT   14 /* two rows */

static const char *TAG = "si5351";

static macjr_tty_t *s_tty;
static volatile bool s_redraw; /* set when the monitor closes */

static bool s_ok;          /* the chip answered */
static bool s_on;          /* outputs enabled */
static uint32_t s_hz[CH];  /* 0: channel off */
static int s_sel;          /* selected channel */
static char s_input[INT_DIGITS + 1 + FRAC_DIGITS + 1];
static const char *s_msg;

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

static void draw_outputs(void) {
  if (!s_ok) {
    line(ROW_OUT, "\x1b[96mOutputs   \x1b[91mSi5351 not found");
  } else {
    line(ROW_OUT, "\x1b[96mOutputs   %s", s_on ? "\x1b[92mON" : "\x1b[91mOFF");
  }
}

static void draw_channels(void) {
  for (int ch = 0; ch < CH; ch++) {
    const char *mark = ch == s_sel ? "\x1b[93m> " : "  ";
    const char *name = ch == s_sel ? "\x1b[93m" : "\x1b[96m";
    if (s_hz[ch]) {
      /* green while it really comes out, grey while the outputs are off */
      line(ROW_CH + ch, "%s%sCLK%d   %s%3lu.%06lu MHz", mark, name, ch, s_on ? "\x1b[92m" : "\x1b[37m",
           (unsigned long)(s_hz[ch] / 1000000), (unsigned long)(s_hz[ch] % 1000000));
    } else {
      line(ROW_CH + ch, "%s%sCLK%d   \x1b[90moff", mark, name, ch);
    }
  }
}

static void draw_input(void) {
  /* right-align the MHz part under the channels' */
  const char *dot = strchr(s_input, '.');
  int int_len = dot ? (int)(dot - s_input) : (int)strlen(s_input);
  char field[INT_DIGITS + 1 + FRAC_DIGITS + 2]; /* the number, the cursor, a space */
  snprintf(field, sizeof(field), "%*s%s_", INT_DIGITS - int_len, "", s_input);
  line(ROW_INPUT, "\x1b[96mInput     \x1b[97m%-*sMHz", (int)sizeof(field) - 1, field);
  line(ROW_MSG, "\x1b[91m%s", s_msg ? s_msg : "");
}

static void draw_all(void) {
  if (monitor_is_open()) {
    return; /* redrawn when it closes */
  }
  out("\x1b[?25l\x1b[0m\x1b[2J");
  macjr_tty_header(s_tty, "Si5351 generator");
  draw_outputs();
  draw_channels();
  draw_input();
  line(ROW_RANGE, "\x1b[90m 0.008 - 100 MHz, 0 turns off");
  line(ROW_HINT, "\x1b[90m 0-9 . MHz  \x98\x83 del  \x82^ set"); /* ШГ В↑ */
  line(ROW_HINT + 1, "\x1b[90m \x8F>x x>\x8F chan  \x91/\x8F on/off  Cx exit"); /* П С/П */
  out("\x1b[0m");
  macjr_display_flush(s_tty);
}

/* Add a typed character to the input if it keeps the form ddd.dddddd. */
static void type(char c) {
  size_t len = strlen(s_input);
  const char *dot = strchr(s_input, '.');
  if (c == '.') {
    if (!dot) {
      strcpy(s_input + len, len ? "." : "0.");
    }
  } else if (dot ? strlen(dot + 1) < FRAC_DIGITS : len < INT_DIGITS) {
    if (len == 1 && s_input[0] == '0') {
      len = 0; /* no leading zeros */
    }
    s_input[len] = c;
    s_input[len + 1] = '\0';
  }
}

/* "14.074" -> 14074000 Hz */
static uint32_t parse_hz(const char *input) {
  uint32_t mhz = 0, frac = 0;
  int frac_n = 0;
  const char *p = input;
  for (; *p && *p != '.'; p++) {
    mhz = mhz * 10 + (uint32_t)(*p - '0');
  }
  if (*p == '.') {
    for (p++; *p; p++, frac_n++) {
      frac = frac * 10 + (uint32_t)(*p - '0');
    }
  }
  for (; frac_n < FRAC_DIGITS; frac_n++) {
    frac *= 10;
  }
  return mhz * 1000000 + frac;
}

/* ---- the config store ---- */

static void key_of(int ch, char *key, size_t len) {
  if (ch < 0) {
    snprintf(key, len, "gen_on");
  } else {
    snprintf(key, len, "gen_f%c", (char)('0' + ch));
  }
}

/* Channel `ch`'s saved frequency (ch -1: the on/off state), or 0. */
static uint32_t load(int ch) {
  char key[12], buf[16];
  key_of(ch, key, sizeof(key));
  if (db_read(key, buf, sizeof(buf)) != ESP_OK) {
    return 0;
  }
  char *end;
  unsigned long v = strtoul(buf, &end, 10);
  if (end == buf || *end || (ch >= 0 && v && (v < SDR_GEN_MIN_HZ || v > SDR_GEN_MAX_HZ))) {
    ESP_LOGW(TAG, "bad %s=%s in the config, ignored", key, buf);
    return 0;
  }
  return (uint32_t)v;
}

static void save(int ch, uint32_t v) {
  char key[12], buf[16];
  key_of(ch, key, sizeof(key));
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);
  esp_err_t err = db_update(key, buf);
  if (err == ESP_ERR_NOT_FOUND) {
    err = db_create(key, buf);
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
  }
}

/* ---- keys ---- */

static void set_freq(void) {
  if (!s_input[0]) {
    return;
  }
  uint32_t hz = parse_hz(s_input);
  if (hz && (hz < SDR_GEN_MIN_HZ || hz > SDR_GEN_MAX_HZ)) {
    s_msg = "Out of range: 0.008-100 MHz";
    return;
  }
  esp_err_t err = s_ok ? sdr_gen_set_freq(s_sel, hz) : ESP_ERR_INVALID_STATE;
  if (err != ESP_OK) {
    s_msg = "Setting failed";
    ESP_LOGE(TAG, "sdr_gen_set_freq(%d, %lu): %s", s_sel, (unsigned long)hz, esp_err_to_name(err));
    return;
  }
  s_hz[s_sel] = hz;
  s_input[0] = '\0';
  save(s_sel, hz);
}

static void toggle(void) {
  esp_err_t err = s_ok ? sdr_gen_enable(!s_on) : ESP_ERR_INVALID_STATE;
  if (err != ESP_OK) {
    s_msg = "Switching failed";
    return;
  }
  s_on = !s_on;
  save(-1, s_on);
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
static bool on_key(uint8_t code) {
  s_msg = NULL;
  int d = digit_of(code);
  if (d >= 0) {
    type((char)('0' + d));
  } else if (code == KEY_MK_DOT) {
    type('.');
  } else if (code == KEY_DEL) {
    size_t len = strlen(s_input);
    if (len) {
      s_input[len - 1] = '\0';
    }
  } else if (code == KEY_UP) {
    s_sel = (s_sel + CH - 1) % CH;
  } else if (code == KEY_DOWN) {
    s_sel = (s_sel + 1) % CH;
  } else if (code == KEY_SET) {
    set_freq();
  } else if (code == KEY_ONOFF) {
    toggle();
  } else if (code == KEY_BACK) {
    if (!s_input[0]) {
      return false;
    }
    s_input[0] = '\0';
  }
  return true;
}

static void on_monitor_closed(void *ctx) {
  (void)ctx;
  s_redraw = true;
}

/* Start the chip and restore the saved channels and on/off state. */
static void start(void) {
  s_ok = sdr_gen_init() == ESP_OK;
  s_on = false;
  for (int ch = 0; ch < CH; ch++) {
    s_hz[ch] = 0;
  }
  if (!s_ok) {
    ESP_LOGE(TAG, "Si5351 init failed");
    return;
  }
  for (int ch = 0; ch < CH; ch++) {
    uint32_t hz = load(ch);
    if (hz && sdr_gen_set_freq(ch, hz) == ESP_OK) {
      s_hz[ch] = hz;
    }
  }
  if (load(-1) && sdr_gen_enable(true) == ESP_OK) {
    s_on = true;
  }
}

int main(int argc, char *argv[], macjr_tty_t *tty) {
  s_tty = tty;
  s_sel = 0;
  s_input[0] = '\0';
  s_msg = NULL;
  start();
  monitor_set_redraw_cb(on_monitor_closed, NULL);
  draw_all();

  int fd = open(KBD_DEV_PATH, O_RDONLY);
  for (;;) {
    int key = -1;
    if (fd >= 0) {
      fd_set keys;
      FD_ZERO(&keys);
      FD_SET(fd, &keys);
      struct timeval timeout = {.tv_sec = 0, .tv_usec = POLL_MS * 1000};
      uint8_t k;
      if (select(fd + 1, &keys, NULL, NULL, &timeout) > 0 && read(fd, &k, 1) == 1) {
        key = k;
      }
    } else {
      sys_delay(POLL_MS);
    }
    if (monitor_is_open()) {
      continue; /* the monitor owns the screen, and the keys with it */
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
  if (fd >= 0) {
    close(fd);
  }
  return 0;
}
