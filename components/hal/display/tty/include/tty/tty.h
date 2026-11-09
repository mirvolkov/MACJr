/*
 * Top-level TTY API: ties the HAL display, the line discipline (termios-like
 * canonical/raw input handling), and the symbolic console together into a
 * single object that behaves like a Linux /dev/ttyN from the caller's side
 * (write() sends process output through the console, read() blocks for a
 * completed line in canonical mode, kbd_inject() feeds in raw keyboard bytes).
 */
#ifndef MACJR_TTY_H
#define MACJR_TTY_H

#include "tty/hal_display.h"
#include "tty/termios.h"
#include "tty/vc.h"

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct macjr_tty macjr_tty_t;

/* Allocate and initialize a tty sized to fit hal's pixel geometry given a
 * glyph_w x glyph_h font cell. Returns NULL on allocation/RTOS-object failure. */
macjr_tty_t *macjr_tty_create(macjr_hal_display_t *hal, int glyph_w, int glyph_h);
/* Free a tty and all of its FreeRTOS objects/buffers. */
void macjr_tty_destroy(macjr_tty_t *tty);

/* Read one line discipline result: blocks for a completed line in canonical
 * mode, or returns raw bytes per VMIN/VTIME semantics in raw mode. */
ssize_t macjr_tty_read(macjr_tty_t *tty, void *buf, size_t len);
/* Write process output (e.g. shell/app text, ANSI escapes) into the console. */
ssize_t macjr_tty_write(macjr_tty_t *tty, const void *buf, size_t len);

/* Keyboard driver calls this (task context recommended). */
void macjr_kbd_inject(macjr_tty_t *tty, const uint8_t *data, size_t len);

/* After writes / injects, call periodically to push dirty cells to the panel. */
void macjr_display_flush(macjr_tty_t *tty);

/* Copy of the whole console (cells, cursor, colors) for overlays such as the
 * system monitor: save it, draw something else, then restore it. */
typedef struct macjr_tty_screen macjr_tty_screen_t;
/* Snapshot the current screen. Returns NULL if out of memory. */
macjr_tty_screen_t *macjr_tty_save_screen(macjr_tty_t *tty);
/* Put a saved screen back, mark every row for redraw and free the snapshot.
 * Call macjr_display_flush() afterwards to show it. */
void macjr_tty_restore_screen(macjr_tty_t *tty, macjr_tty_screen_t *screen);

/* Accessors for the underlying console/termios/winsize state. */
macjr_vc_t *macjr_tty_vc(macjr_tty_t *tty);
struct macjr_termios *macjr_tty_termios(macjr_tty_t *tty);
struct macjr_winsize *macjr_tty_winsize(macjr_tty_t *tty);
/* The display the tty draws on. Only for apps agreed to draw graphics
 * (the PNG viewer); console apps stay with the tty. */
macjr_hal_display_t *macjr_tty_display(macjr_tty_t *tty);

/* App header bar character: CP866 double horizontal line. */
#define MACJR_TTY_HEADER_CHAR '\xCD'

/* Draw the app header on the top row, the title in brackets centered
 * between bars across the whole width, as the navigator's top edge (grey
 * bars, the title white on black):
 *
 *   ═════════════[SoftRock SDR]═════════════
 *
 * `title` is plain CP866 text without escapes. Leaves the cursor at the
 * start of row 2 and the colors reset; doesn't flush. */
void macjr_tty_header(macjr_tty_t *tty, const char *title);

#ifdef __cplusplus
}
#endif

#endif
