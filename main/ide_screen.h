/*
 * Demo screen content: a one-shot, static recreation of a classic Turbo C
 * IDE, drawn directly at the console's fixed 40x15 size.
 */
#ifndef MACJR_IDE_SCREEN_H
#define MACJR_IDE_SCREEN_H

#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MACJR_IDE_COLS 40
#define MACJR_IDE_ROWS 15

/* Clears nothing itself -- caller is expected to have cleared/hidden the
 * cursor first (see main.c). Draws the menu bar, the
 * double-line-bordered editor window with its "Edit" tab, and the
 * single-line-bordered "Linking" dialog (flash/mem fields queried live from
 * the chip), then leaves it on screen -- no animation. */
void macjr_draw_turboc_ide(macjr_tty_t *tty);

#ifdef __cplusplus
}
#endif

#endif
