/*
 * MK-52 calculator app: draws the screen and feeds /dev/kbrd keys to the
 * calculator core (mk52_core.c).
 *
 *    Elektronika MK-52
 *   ────────────────────────────────────────
 *
 *     -1.2345678-01               <- the VFD digit line, 24x64 per char
 *
 *   AUTO  F  DEG
 *
 * The header and status line are tty text. The digit line uses the 8x16
 * font scaled 3x wide and 4x tall (24x64; 4x wide would not fit 13
 * characters into 320 px) and is drawn straight onto the display HAL over
 * text rows 3-6, which the tty leaves blank so it never paints over it.
 *
 * While a program runs the digit line is blank (the real VFD flickers) and
 * the core is stepped in 20 ms bursts between key checks, so С/П can stop
 * it and other tasks still get the CPU.
 */
#include "mk52.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard.h"
#include "mk52_core.h"

#define RUN_SLICE_US 20000
#define STATUS_ROW 8

#define BIG_SX 3       /* 8x16 font scaled 3x wide, 4x tall */
#define BIG_SY 4
#define BIG_W (8 * BIG_SX)
#define BIG_H (16 * BIG_SY)
#define BIG_ROW_BYTES ((BIG_W + 7) / 8)
#define BIG_Y 32       /* text rows 3-6 */
#define BIG_RIGHT 316  /* right edge of the right-aligned digit line, px */
#define COLOR_DIGITS 0x55FF55
#define COLOR_BG 0x000000

#define CH_HLINE "\xC4"

extern const uint8_t macjr_font_builtin_8x16[256][16];

static const char *TAG = "mk52";

static macjr_tty_t *s_tty;
static macjr_hal_display_t *s_display;

/* /dev/kbrd code -> calculator key; see the layout table in keyboard.h. */
static int key_of(uint8_t code) {
  switch (code) {
  case KEY_MK_0: return MK_KEY_0;
  case KEY_MK_1: return MK_KEY_1;
  case KEY_MK_2: return MK_KEY_2;
  case KEY_MK_3: return MK_KEY_3;
  case KEY_MK_4: return MK_KEY_4;
  case KEY_MK_5: return MK_KEY_5;
  case KEY_MK_6: return MK_KEY_6;
  case KEY_MK_7: return MK_KEY_7;
  case KEY_MK_8: return MK_KEY_8;
  case KEY_MK_9: return MK_KEY_9;
  case KEY_MK_DOT: return MK_KEY_DOT;
  case KEY_MK_NEG: return MK_KEY_NEG;
  case KEY_MK_EE: return MK_KEY_EE;
  case KEY_MK_CX: return MK_KEY_CX;
  case KEY_MK_ENTER: return MK_KEY_ENTER;
  case KEY_MK_SWAP: return MK_KEY_SWAP;
  case KEY_MK_PLUS: return MK_KEY_PLUS;
  case KEY_MK_MINUS: return MK_KEY_MINUS;
  case KEY_MK_MUL: return MK_KEY_MUL;
  case KEY_MK_DIV: return MK_KEY_DIV;
  case KEY_MK_F: return MK_KEY_F;
  case KEY_MK_K: return MK_KEY_K;
  case KEY_MK_X_TO_P: return MK_KEY_STO;
  case KEY_MK_P_TO_X: return MK_KEY_RCL;
  case KEY_MK_STEP_FWD: return MK_KEY_SST;
  case KEY_MK_STEP_BCK: return MK_KEY_BST;
  case KEY_MK_BP: return MK_KEY_BP;
  case KEY_MK_PP: return MK_KEY_PP;
  case KEY_MK_VO: return MK_KEY_VO;
  case KEY_MK_SP: return MK_KEY_SP;
  case KEY_MK_A_UP: return MK_KEY_A_UP;
  case KEY_MK_UPDOWN: return MK_KEY_UPDOWN;
  default: return -1;
  }
}

static void out(const char *s) {
  macjr_tty_write(s_tty, s, strlen(s));
}

static void draw_static(void) {
  char rule[40 * sizeof(CH_HLINE) + 1] = "";
  for (int i = 0; i < 40; i++) {
    strcat(rule, CH_HLINE);
  }
  out("\x1b[?25l\x1b[0m\x1b[2J");
  out("\x1b[1;1H\x1b[92m Elektronika MK-52");
  out("\x1b[2;1H\x1b[94m");
  out(rule);
}

/* Draw `s` right-aligned, each 8x16 glyph pixel becoming BIG_SX x BIG_SY. */
static void draw_big(const char *s) {
  macjr_hal_display_t *d = s_display;
  uint8_t big[BIG_H][BIG_ROW_BYTES];
  int x = BIG_RIGHT - (int)strlen(s) * BIG_W;

  if (d->begin_frame) d->begin_frame(d);
  d->fill_rect(d, 0, BIG_Y, d->width_px, BIG_H, COLOR_BG);
  for (; *s; s++, x += BIG_W) {
    const uint8_t *g = macjr_font_builtin_8x16[(uint8_t)*s];
    memset(big, 0, sizeof(big));
    for (int yy = 0; yy < BIG_H; yy++) {
      uint8_t src = g[yy / BIG_SY];
      for (int xx = 0; xx < BIG_W; xx++) {
        if (src & (0x80 >> (xx / BIG_SX))) {
          big[yy][xx >> 3] |= 0x80 >> (xx & 7);
        }
      }
    }
    d->blit_1bpp_mask(d, x, BIG_Y, BIG_W, BIG_H, BIG_ROW_BYTES, &big[0][0], COLOR_DIGITS, COLOR_BG);
  }
  if (d->end_frame) d->end_frame(d);
}

static void draw_state(const mk52_t *m) {
  static const char *angle[] = {"RAD", "GRD", "DEG"};
  char disp[32] = "";
  char line[64];

  snprintf(line, sizeof(line), "\x1b[%d;1H\x1b[K\x1b[97m %-4s  \x1b[93m%c  \x1b[97m%s  \x1b[91m%s",
           STATUS_ROW, m->mode == MK_MODE_PRG ? "PRG" : "AUTO", m->prefix ? m->prefix : ' ',
           angle[m->angle], m->running ? "RUN" : "");
  out(line);
  macjr_display_flush(s_tty);

  if (!m->running) {
    mk52_display(m, disp, sizeof(disp));
  }
  draw_big(disp);
}

void mk52_run(macjr_tty_t *tty, macjr_hal_display_t *display) {
  static mk52_t m;
  s_tty = tty;
  s_display = display;
  mk52_reset(&m);

  int fd = open(KBD_DEV_PATH, O_RDONLY);
  if (fd < 0) {
    ESP_LOGE(TAG, "open %s failed: errno %d", KBD_DEV_PATH, errno);
  }
  draw_static();
  draw_state(&m);

  for (;;) {
    uint8_t code;

    if (m.running) {
      int64_t until = esp_timer_get_time() + RUN_SLICE_US;
      while (m.running && esp_timer_get_time() < until) {
        mk52_step(&m);
      }
      if (fd >= 0 && read(fd, &code, 1) == 1 && key_of(code) >= 0) {
        mk52_key(&m, (mk52_key_t)key_of(code));
      }
      if (!m.running) {
        fcntl(fd, F_SETFL, 0);
        draw_state(&m);
      }
      vTaskDelay(1);
      continue;
    }

    if (fd < 0 || read(fd, &code, 1) != 1) { /* blocks until a key */
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    int key = key_of(code);
    if (key < 0) {
      continue;
    }
    mk52_key(&m, (mk52_key_t)key);
    if (m.running) {
      fcntl(fd, F_SETFL, O_NONBLOCK);
    }
    draw_state(&m);
  }
}
