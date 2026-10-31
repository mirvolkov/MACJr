/*
 * SoftRock SDR app. The Si5351 drives the Tayloe detector (components/hal/
 * sdr) and the ADC streams I/Q to the host as a USB microphone
 * (components/hal/mic). The screen shows the frequency the LO runs at and
 * the one being typed:
 *
 *    SoftRock SDR
 *   ────────────────────────────────────────
 *   Active    7.055000 MHz       (green)
 *   Input    14.074_   MHz
 *   Out of range: 4.762-28 MHz   (red, after a bad В↑)
 *
 *   LO      Si5351 I/Q
 *   UAC     streaming I/Q as USB mic
 *
 * UAC takes the native USB port away from USB-Serial-JTAG, so this mode is
 * only entered from the boot menu; the log still goes out of UART0.
 *
 * The active frequency is kept in the config store (components/hal/db) as
 * sdr_freq=<Hz> and restored at the next start.
 */
#include "radio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard.h"
#include "mic.h"
#include "monitor.h"
#include "sdr.h"

#define INT_DIGITS 3  /* MHz part: up to 150 */
#define FRAC_DIGITS 6 /* down to 1 Hz */
#define KEY_SET KEY_MK_ENTER    /* В↑ */
#define KEY_DEL KEY_MK_STEP_BCK /* ←ШГ (Cx belongs to the monitor) */
#define CH_HLINE "\xC4"
/* Tuning range: the LO's quadrature floor up to the 10 m band */
#define RADIO_MIN_HZ SDR_MIN_FREQ_HZ
#define RADIO_MAX_HZ 28000000u
#define DB_KEY_FREQ "sdr_freq" /* active frequency in Hz, e.g. 7055000 */

#define ROW_ACTIVE 3
#define ROW_INPUT 4
#define ROW_MSG 5
#define ROW_LO 7
#define ROW_UAC 8
#define ROW_RANGE 14
#define ROW_HINT 15

static const char *TAG = "radio";

static macjr_tty_t *s_tty;

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

static void draw_active(uint32_t hz, bool on) {
  if (on) {
    line(ROW_ACTIVE, "\x1b[96mActive  \x1b[92m%3lu.%06lu MHz", (unsigned long)(hz / 1000000),
         (unsigned long)(hz % 1000000));
  } else {
    line(ROW_ACTIVE, "\x1b[96mActive  \x1b[91m---");
  }
}

static void draw_input(const char *input) {
  /* right-align the MHz part under the active frequency's */
  const char *dot = strchr(input, '.');
  int int_len = dot ? (int)(dot - input) : (int)strlen(input);
  char field[INT_DIGITS + 1 + FRAC_DIGITS + 2]; /* the number, the cursor, a space */
  snprintf(field, sizeof(field), "%*s%s_", INT_DIGITS - int_len, "", input);
  line(ROW_INPUT, "\x1b[96mInput   \x1b[97m%-*sMHz", (int)sizeof(field) - 1, field);
}

static void draw_msg(const char *msg) {
  line(ROW_MSG, "\x1b[91m%s", msg ? msg : "");
}

static void draw_static(bool lo_ok, bool uac_ok) {
  char rule[40 * sizeof(CH_HLINE) + 1] = "";
  for (int i = 0; i < 40; i++) {
    strcat(rule, CH_HLINE);
  }
  out("\x1b[?25l\x1b[0m\x1b[2J");
  line(1, "\x1b[93m SoftRock SDR");
  line(2, "\x1b[94m%s", rule);
  line(ROW_LO, lo_ok ? "\x1b[96mLO      \x1b[97mSi5351 I/Q" : "\x1b[96mLO      \x1b[91mSi5351 init failed");
  line(ROW_UAC, uac_ok ? "\x1b[96mUAC     \x1b[97mstreaming I/Q as USB mic"
                       : "\x1b[96mUAC     \x1b[91mmic init failed");
  /* 0-9 . MHz   ШГ delete   В^ set */
  line(ROW_RANGE, "\x1b[90m Tune 4.762 - 28 MHz");
  line(ROW_HINT, "\x1b[90m 0-9 . MHz   \x98\x83 delete   \x82^ set");
}

/* Add a typed character to `input` if it keeps the form ddd.dddddd. */
static void type(char *input, char c) {
  size_t len = strlen(input);
  const char *dot = strchr(input, '.');
  if (c == '.') {
    if (!dot) {
      strcpy(input + len, len ? "." : "0.");
    }
  } else if (dot ? strlen(dot + 1) < FRAC_DIGITS : len < INT_DIGITS) {
    if (len == 1 && input[0] == '0') {
      len = 0; /* no leading zeros */
    }
    input[len] = c;
    input[len + 1] = '\0';
  }
}

/* "14.074" -> 14074000 Hz; 0 if nothing was typed. */
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

/* The frequency saved by save_freq(), or SDR_DEFAULT_FREQ_HZ if there is
 * none (no card, no key, or not a number). */
static uint32_t load_freq(void) {
  char buf[16];
  if (db_read(DB_KEY_FREQ, buf, sizeof(buf)) != ESP_OK) {
    return SDR_DEFAULT_FREQ_HZ;
  }
  char *end;
  unsigned long hz = strtoul(buf, &end, 10);
  if (end == buf || *end || hz < RADIO_MIN_HZ || hz > RADIO_MAX_HZ) {
    ESP_LOGW(TAG, "bad %s=%s in the config, using the default", DB_KEY_FREQ, buf);
    return SDR_DEFAULT_FREQ_HZ;
  }
  return (uint32_t)hz;
}

static void save_freq(uint32_t hz) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)hz);
  esp_err_t err = db_update(DB_KEY_FREQ, buf);
  if (err == ESP_ERR_NOT_FOUND) {
    err = db_create(DB_KEY_FREQ, buf);
  }
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "saving %s failed: %s", DB_KEY_FREQ, esp_err_to_name(err));
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

void radio_run(macjr_tty_t *tty) {
  s_tty = tty;

  bool lo_ok = sdr_init() == ESP_OK; /* starts at SDR_DEFAULT_FREQ_HZ */
  uint32_t active = SDR_DEFAULT_FREQ_HZ;
  uint32_t saved = load_freq();
  if (lo_ok && saved != active && sdr_set_freq(saved) == ESP_OK) {
    active = saved;
  }
  bool uac_ok = mic_init() == ESP_OK && mic_uac_start() == ESP_OK;
  if (!lo_ok) {
    ESP_LOGE(TAG, "Si5351 init failed");
  }
  if (!uac_ok) {
    ESP_LOGE(TAG, "mic init failed");
  }

  char input[INT_DIGITS + 1 + FRAC_DIGITS + 1] = "";
  draw_static(lo_ok, uac_ok);
  draw_active(active, lo_ok);
  draw_input(input);
  macjr_display_flush(tty);

  int fd = open(KBD_DEV_PATH, O_RDONLY);
  if (fd < 0) {
    ESP_LOGE(TAG, "open %s failed: errno %d", KBD_DEV_PATH, errno);
  }
  for (;;) {
    uint8_t code;
    if (fd < 0 || read(fd, &code, 1) != 1) { /* blocks until a key */
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (monitor_is_open()) {
      continue; /* the monitor owns the screen, and the keys with it */
    }

    const char *msg = NULL;
    int d = digit_of(code);
    if (d >= 0) {
      type(input, (char)('0' + d));
    } else if (code == KEY_MK_DOT) {
      type(input, '.');
    } else if (code == KEY_DEL) {
      size_t len = strlen(input);
      if (len) {
        input[len - 1] = '\0';
      }
    } else if (code == KEY_SET) {
      if (!input[0]) {
        continue;
      }
      uint32_t hz = parse_hz(input);
      esp_err_t err = hz < RADIO_MIN_HZ || hz > RADIO_MAX_HZ ? ESP_ERR_INVALID_SIZE : sdr_set_freq(hz);
      if (err == ESP_OK) {
        active = hz;
        input[0] = '\0';
        save_freq(hz);
        ESP_LOGI(TAG, "LO at %lu Hz", (unsigned long)hz);
      } else if (err == ESP_ERR_INVALID_SIZE) {
        msg = "Out of range: 4.762-28 MHz";
      } else {
        msg = "Tuning failed";
        ESP_LOGE(TAG, "sdr_set_freq(%lu): %s", (unsigned long)hz, esp_err_to_name(err));
      }
      draw_active(active, lo_ok);
    } else {
      continue;
    }
    draw_input(input);
    draw_msg(msg);
    macjr_display_flush(tty);
  }
}
