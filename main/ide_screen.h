/*
 * Portable (no ESP-IDF, no host-specific) screen content: a one-shot,
 * static recreation of a classic Turbo C IDE, drawn directly at the
 * console's fixed 40x15 size. Shared by both the ESP32 and host entry
 * points (main_esp32.c / main_macos.c).
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
 * cursor first (see main_esp32.c / main_macos.c). Draws the menu bar, the
 * double-line-bordered editor window with its "Edit" tab, and the
 * single-line-bordered "Linking" dialog (flash/mem fields queried live via
 * platform_stats.h), then leaves it on screen -- no animation. */
void macjr_draw_turboc_ide(macjr_tty_t *tty);

#ifdef __cplusplus
}
#endif

#endif
