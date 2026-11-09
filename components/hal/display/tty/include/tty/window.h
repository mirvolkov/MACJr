/*
 * Dialog window for console apps: a double-lined box drawn over the
 * screen, cyan on blue, with an optional title in its top edge in the
 * style of the app header, and lines of text inside.
 *
 *   ╔══════[Screenshot taken]══════╗
 *   ║                              ║
 *   ║      SHOTS/SHOT0008.PNG      ║
 *   ║                              ║
 *   ╚══════════════════════════════╝
 *
 * The window only draws: whatever it covers is the caller's to restore
 * (macjr_tty_save_screen() / macjr_tty_restore_screen(), or a redraw).
 */
#ifndef MACJR_TTY_WINDOW_H
#define MACJR_TTY_WINDOW_H

#include <stdbool.h>

#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int row, col;      /* top-left cell, 1-based */
  int width, height; /* in cells, frame included */
} macjr_tty_window_t;

/* Place a window of `width` x `height` cells in the middle of the screen.
 * Draws nothing. (Set the fields directly for any other place.) */
void macjr_tty_window_center(macjr_tty_t *tty, macjr_tty_window_t *win, int width, int height);

/* Draw the frame, with `title` (plain CP866, NULL for none) as "[title]"
 * in the middle of the top edge, white, and blank the inside. Leaves the
 * colors reset; doesn't flush. */
void macjr_tty_window_draw(macjr_tty_t *tty, const macjr_tty_window_t *win, const char *title);

/* Write `text` (plain CP866) on inside line `line` (0 is the first row
 * under the top edge), one space in from the frame, cut to fit and padded
 * to the frame; centered if `center`. `fg` is the SGR foreground ("97"
 * white, "93" yellow, ...; NULL for white) on the window's blue. Leaves the
 * colors reset; doesn't flush. */
void macjr_tty_window_text(macjr_tty_t *tty, const macjr_tty_window_t *win, int line, const char *fg, bool center,
                           const char *text);

#ifdef __cplusplus
}
#endif

#endif
