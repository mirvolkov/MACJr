/*
 * PLASMA.ELF -- screen animation until any key, in one of two modes:
 *
 *   -m  matrix: green glyphs rain down the columns (the default, which
 *       the boot menu runs: it passes no arguments)
 *   -r  rainbow: random glyphs in random colors, frame after frame
 *
 *   ════════════════ PLASMA ════════════════
 *   rows 2-14: random glyphs
 *   row 15:    hint
 */
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "keyboard.h"
#include "macjr.h"
#include "monitor.h"

#define TOP_ROW 2
#define HINT_ROW 15
#define W 40
#define H (HINT_ROW - TOP_ROW) /* 13 rows: the last screen cell would scroll */
#define FRAME_MS 50
#define FLICKER 6 /* matrix: trail glyphs changed per frame */

/* Matrix colors: the head of a drop, the cell behind it, the rest. */
#define C_HEAD 97 /* bright white */
#define C_NECK 92 /* bright green */
#define C_TAIL 32 /* green */

static uint32_t s_seed;

/* xorshift32: the loader exports no rand() */
static uint32_t rnd(void) {
  s_seed ^= s_seed << 13;
  s_seed ^= s_seed >> 17;
  s_seed ^= s_seed << 5;
  return s_seed;
}

static void out(macjr_tty_t *tty, const char *s) {
  macjr_tty_write(tty, s, strlen(s));
}

/* Printable ASCII or a CP866 shade/box-drawing glyph (0xB0-0xDF). */
static char random_glyph(void) {
  uint32_t r = rnd() % (94 + 48);
  return (char)(r < 94 ? '!' + r : 0xB0 + (r - 94));
}

static void draw_rainbow(macjr_tty_t *tty) {
  char line[W * 6 + 16]; /* a color change and a glyph per cell at most */
  for (int y = 0; y < H; y++) {
    int n = snprintf(line, sizeof(line), "\x1b[%d;1H", TOP_ROW + y);
    for (int x = 0; x < W; x++) {
      n += snprintf(line + n, sizeof(line) - n, "\x1b[%um", 91 + (unsigned)(rnd() % 7)); /* bright colors */
      line[n++] = random_glyph();
    }
    macjr_tty_write(tty, line, n);
  }
}

/* ---- matrix ------------------------------------------------------------ */

/* A drop falling down a column: its head at row `head` (negative while it
 * waits above the screen), `len` cells of trail, one row per `speed`
 * frames. */
typedef struct {
  int head;
  int len;
  int speed;
  int tick;
} drop_t;

static drop_t s_drops[W];
static char s_glyph[H][W]; /* what the screen shows, ' ' = empty */
static char s_out[4096];
static int s_n;

/* Digits, Latin capitals and CP866 Cyrillic capitals. */
static char matrix_glyph(void) {
  uint32_t r = rnd() % (10 + 26 + 32);
  return (char)(r < 10 ? '0' + r : r < 36 ? 'A' + (r - 10) : 0x80 + (r - 36));
}

static void new_drop(drop_t *d) {
  d->len = 4 + (int)(rnd() % 9);
  d->head = -(int)(rnd() % 16); /* a random pause before it shows */
  d->speed = 1 + (int)(rnd() % 3);
  d->tick = 0;
}

static void put(macjr_tty_t *tty, int x, int y, char ch, int color) {
  if (y < 0 || y >= H) {
    return;
  }
  if (s_n > (int)sizeof(s_out) - 24) {
    macjr_tty_write(tty, s_out, s_n);
    s_n = 0;
  }
  s_glyph[y][x] = ch;
  s_n += snprintf(s_out + s_n, sizeof(s_out) - s_n, "\x1b[%d;%dH\x1b[%dm", TOP_ROW + y, x + 1, color);
  s_out[s_n++] = ch;
}

static void matrix_init(void) {
  memset(s_glyph, ' ', sizeof(s_glyph));
  for (int x = 0; x < W; x++) {
    new_drop(&s_drops[x]);
  }
}

static void draw_matrix(macjr_tty_t *tty) {
  s_n = 0;
  for (int x = 0; x < W; x++) {
    drop_t *d = &s_drops[x];
    if (++d->tick < d->speed) {
      continue;
    }
    d->tick = 0;
    d->head++;
    put(tty, x, d->head, matrix_glyph(), C_HEAD);
    put(tty, x, d->head - 1, matrix_glyph(), C_NECK);
    if (d->head - 2 >= 0 && d->head - 2 < H) {
      put(tty, x, d->head - 2, s_glyph[d->head - 2][x], C_TAIL);
    }
    put(tty, x, d->head - d->len, ' ', C_TAIL);
    if (d->head - d->len >= H - 1) {
      new_drop(d); /* the trail has left the screen */
    }
  }
  /* Trails flicker: a few of their glyphs change. */
  for (int i = 0; i < FLICKER; i++) {
    int x = (int)(rnd() % W);
    int y = (int)(rnd() % H);
    int behind = s_drops[x].head - y; /* 2.. inside the trail */
    if (s_glyph[y][x] != ' ' && behind >= 2) {
      put(tty, x, y, matrix_glyph(), C_TAIL);
    }
  }
  macjr_tty_write(tty, s_out, s_n);
}

/* ---- main ---------------------------------------------------------------- */

int main(int argc, char *argv[], macjr_tty_t *tty) {
  bool rainbow = false;
  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-' && argv[i][1] == 'r') {
      rainbow = true;
    } else if (argv[i][0] == '-' && argv[i][1] == 'm') {
      rainbow = false;
    }
  }

  int fd = open(KBD_DEV_PATH, O_RDONLY);
  if (fd >= 0) {
    fcntl(fd, F_SETFL, O_NONBLOCK);
  }
  s_seed = sys_millis() | 1; /* xorshift needs a non-zero seed */
  matrix_init();

  out(tty, "\x1b[?25l\x1b[0m\x1b[2J");
  macjr_tty_header(tty, "PLASMA");
  char hint[32];
  snprintf(hint, sizeof(hint), "\x1b[%d;1H\x1b[90m Any key: exit", HINT_ROW);
  out(tty, hint);

  for (;;) {
    uint8_t key;
    if (fd >= 0 && read(fd, &key, 1) == 1 && key != MONITOR_KEY && !monitor_is_open()) {
      break;
    }
    if (!monitor_is_open()) { /* the monitor owns the screen while open */
      if (rainbow) {
        draw_rainbow(tty);
      } else {
        draw_matrix(tty);
      }
      macjr_display_flush(tty);
    }
    sys_delay(FRAME_MS);
  }

  if (fd >= 0) {
    close(fd);
  }
  out(tty, "\x1b[0m");
  return 0;
}
