/*
 * macOS/SDL2 demo entry point: the desktop counterpart to main_esp32.c.
 * Brings up the SDL2-backed display HAL (components/macos_hal)
 * and the same macjr_tty TTY stack, draws the portable Turbo C IDE screen
 * (ide_screen.c, unmodified from the ESP32 build), then idles -- pumping
 * window events -- until the window is closed.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tty/tty.h"
#include "display_hal.h"
#include "ide_screen.h"

#define MACJR_PANEL_W 320
#define MACJR_PANEL_H 240

static int64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int main(void) {
  macjr_hal_display_t *hal = macos_display_hal_init(MACJR_PANEL_W, MACJR_PANEL_H, "MACJr (macOS host)");
  if (!hal) {
    fprintf(stderr, "macos_display_hal_init failed\n");
    return 1;
  }

  macjr_tty_t *tty = macjr_tty_create(hal, 8, 16); /* matches the built-in 8x16 font */
  if (!tty) {
    fprintf(stderr, "macjr_tty_create failed\n");
    macos_display_hal_shutdown(hal);
    return 1;
  }

  struct macjr_winsize *ws = macjr_tty_winsize(tty);
  printf("tty up: %ux%u chars on %ux%u px\n", ws->ws_col, ws->ws_row, ws->ws_xpixel, ws->ws_ypixel);
  fflush(stdout);

  /* Static, one-shot recreation of a classic Turbo C IDE screen -- same
   * content as the ESP32 build, just rendered into the SDL2 window. */
  const char *clear = "\x1b[2J\x1b[?25l";
  macjr_tty_write(tty, clear, strlen(clear));
  macjr_draw_turboc_ide(tty);

  int64_t flush_start_us = now_us();
  macjr_display_flush(tty);
  int64_t flush_us = now_us() - flush_start_us;
  printf("display flush took %lld.%03lld ms\n", flush_us / 1000, flush_us % 1000);
  fflush(stdout);

  /* No keyboard input, no redraw loop -- just keep the window responsive
   * (so macOS doesn't flag it as hung) until the user closes it. */
  while (!macos_display_hal_should_close(hal)) {
    macos_display_hal_pump(hal);
    usleep(16000); /* ~60Hz event-poll cadence */
  }

  macjr_tty_destroy(tty);
  macos_display_hal_shutdown(hal);
  return 0;
}
