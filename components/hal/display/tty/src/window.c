/*
 * Dialog window (tty/window.h).
 */
#include "tty/window.h"

#include <stdio.h>
#include <string.h>

#define COLOR_FRAME "\x1b[0;96;44m" /* cyan on blue */
#define COLOR_TITLE "\x1b[97m"      /* white, as in the app header */
#define BG          "44"

#define G_TL '\xC9' /* ╔ */
#define G_TR '\xBB' /* ╗ */
#define G_BL '\xC8' /* ╚ */
#define G_BR '\xBC' /* ╝ */
#define G_H  '\xCD' /* ═ */
#define G_V  '\xBA' /* ║ */

#define WIN_MAX 64 /* widest window, in cells */

static void write_str(macjr_tty_t *tty, const char *s) {
  macjr_tty_write(tty, s, strlen(s));
}

static void goto_cell(macjr_tty_t *tty, int row, int col) {
  char buf[16];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
  write_str(tty, buf);
}

static int clamp_width(const macjr_tty_window_t *win) {
  return win->width > WIN_MAX ? WIN_MAX : win->width;
}

/* One row of the frame: `l`, `fill` across, `r`. */
static void frame_row(macjr_tty_t *tty, const macjr_tty_window_t *win, int row, char l, char fill, char r) {
  int w = clamp_width(win);
  char buf[WIN_MAX + 1];
  memset(buf, fill, w);
  buf[0] = l;
  buf[w - 1] = r;
  buf[w] = '\0';
  goto_cell(tty, row, win->col);
  write_str(tty, COLOR_FRAME);
  write_str(tty, buf);
}

void macjr_tty_window_center(macjr_tty_t *tty, macjr_tty_window_t *win, int width, int height) {
  struct macjr_winsize *ws = macjr_tty_winsize(tty);
  win->width = width;
  win->height = height;
  win->row = (ws->ws_row - height) / 2 + 1;
  win->col = (ws->ws_col - width) / 2 + 1;
}

void macjr_tty_window_draw(macjr_tty_t *tty, const macjr_tty_window_t *win, const char *title) {
  int w = clamp_width(win);
  if (w < 4 || win->height < 2) {
    return;
  }
  frame_row(tty, win, win->row, G_TL, G_H, G_TR);
  if (title && *title) {
    int len = (int)strnlen(title, w - 4); /* "═[" and "]═" stay */
    goto_cell(tty, win->row, win->col + (w - len - 2) / 2);
    char buf[WIN_MAX + 32];
    snprintf(buf, sizeof(buf), "[" COLOR_TITLE "%.*s" COLOR_FRAME "]", len, title);
    write_str(tty, buf);
  }
  for (int r = 1; r < win->height - 1; r++) {
    frame_row(tty, win, win->row + r, G_V, ' ', G_V);
  }
  frame_row(tty, win, win->row + win->height - 1, G_BL, G_H, G_BR);
  write_str(tty, "\x1b[0m");
}

void macjr_tty_window_text(macjr_tty_t *tty, const macjr_tty_window_t *win, int line, const char *fg, bool center,
                           const char *text) {
  int w = clamp_width(win) - 4; /* "║ " and " ║" */
  if (w <= 0 || line < 0 || line >= win->height - 2) {
    return;
  }
  int len = (int)strnlen(text, w);
  int left = center ? (w - len) / 2 : 0;
  char buf[WIN_MAX + 48];
  snprintf(buf, sizeof(buf), "\x1b[0;%s;" BG "m%*s%.*s%*s\x1b[0m", fg ? fg : "97", left, "", len, text,
           w - len - left, "");
  goto_cell(tty, win->row + 1 + line, win->col + 2);
  write_str(tty, buf);
}
