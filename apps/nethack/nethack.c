/*
 * NETHACK.ELF -- NetHack 3.6 (vendor/nethack) on the 40x15 screen.
 *
 * The game is NetHack's own tty port, unchanged in how it draws: it
 * writes ANSI escapes to stdout and reads keys with tgetch(), and the
 * MacJr system layer (vendor/nethack/sys/macjr) puts both on this
 * program's tty. The screen is smaller than the 80x24 NetHack wants,
 * so the map is clipped and scrolls to follow the hero.
 *
 * The playground is /sdcard/NETHACK (data, saves, scores, .nethackrc),
 * put there by `make install`.
 *
 *   nethack [-u name] [-p role] [-r race] [-@] [-D] [-X]
 */
#include "macjr.h"

void macjr_tty_attach(macjr_tty_t *tty);   /* sys/macjr/macjrtty.c */
int nethack_main(int argc, char *argv[]); /* sys/macjr/macjrunix.c */

/* NetHack's deepest calls (travel, dungeon setup, special levels) need
   well over the 16 KB a program gets by default */
MACJR_STACK(64 * 1024);

int main(int argc, char *argv[], macjr_tty_t *tty) {
  macjr_tty_attach(tty);
  macjr_tty_write(tty, "\x1b[0m\x1b[2J\x1b[H", 11);
  return nethack_main(argc, argv);
}
