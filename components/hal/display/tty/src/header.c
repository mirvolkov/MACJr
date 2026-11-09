/*
 * App header line: the title in brackets centered between bars across the
 * top row, the common first line of the console apps' screens. Same style
 * as the navigator's top edge: grey ═, the title white on black.
 */
#include "tty/tty.h"

#include <string.h>

#define COLOR_BARS  "\x1b[0;37m"
#define COLOR_TITLE "\x1b[0;97;40m"

void macjr_tty_header(macjr_tty_t *tty, const char *title) {
  char bars[64];
  memset(bars, MACJR_TTY_HEADER_CHAR, sizeof(bars));

  int cols = macjr_tty_winsize(tty)->ws_col;
  if (cols > (int)sizeof(bars)) {
    cols = sizeof(bars);
  }
  int len = (int)strlen(title);
  if (len > cols - 2) {
    len = cols - 2; /* no room for bars, keep the brackets */
  }
  int left = (cols - len - 2) / 2;
  int right = cols - len - 2 - left;

  const char *home = "\x1b[1;1H\x1b[0m" COLOR_BARS;
  macjr_tty_write(tty, home, strlen(home));
  macjr_tty_write(tty, bars, left);
  macjr_tty_write(tty, "[" COLOR_TITLE, 1 + strlen(COLOR_TITLE));
  macjr_tty_write(tty, title, len);
  macjr_tty_write(tty, COLOR_BARS "]", strlen(COLOR_BARS) + 1);
  macjr_tty_write(tty, bars, right);
  /* a full row wraps the cursor to row 2; place it there explicitly in
   * case the tty is narrower or wider than 40 */
  const char *done = "\x1b[0m\x1b[2;1H";
  macjr_tty_write(tty, done, strlen(done));
}
