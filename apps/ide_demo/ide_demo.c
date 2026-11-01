/*
 * IDE_DEMO.ELF ("IDE DEMO" in the menu) -- a static recreation of a
 * classic Turbo C IDE screen (ide_screen.c), drawn once and left on screen
 * until В↑.
 */
#include <stdio.h>
#include <string.h>

#include "ide_screen.h"
#include "macjr.h"

int main(int argc, char *argv[], macjr_tty_t *tty) {
  const char *clear = "\x1b[2J\x1b[?25l";
  macjr_tty_write(tty, clear, strlen(clear));
  macjr_draw_turboc_ide(tty);

  unsigned start = sys_millis();
  macjr_display_flush(tty);
  printf("ide_demo: display flush took %u ms\n", sys_millis() - start);

  while (sys_getkey() != SYS_KEY_ENTER) {
  }
  return 0;
}
