/*
 * PLASMA.ELF -- fills the screen with random glyphs in random colors,
 * frame after frame, until any key.
 *
 *   ════════════════ PLASMA ════════════════
 *   rows 2-14: random glyphs
 *   row 15:    hint
 */
#include <fcntl.h>
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

static void draw_frame(macjr_tty_t *tty) {
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

int main(int argc, char *argv[], macjr_tty_t *tty) {
  int fd = open(KBD_DEV_PATH, O_RDONLY);
  if (fd >= 0) {
    fcntl(fd, F_SETFL, O_NONBLOCK);
  }
  s_seed = sys_millis() | 1; /* xorshift needs a non-zero seed */

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
      draw_frame(tty);
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
