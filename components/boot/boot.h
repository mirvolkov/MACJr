#ifndef MACJR_BOOT_H
#define MACJR_BOOT_H

#include "keyboard.h"
#include "tty/hal_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MK-52 digit keys that pick menu entries. */
#define BOOT_MENU_KEY_1 KEY_MK_1
#define BOOT_MENU_KEY_2 KEY_MK_2
#define BOOT_MENU_KEY_3 KEY_MK_3

typedef enum {
  BOOT_MODE_TTY_DEMO = 1, /* default: TTY + Turbo C IDE demo screen */
  BOOT_MODE_SDR_UAC = 2,  /* Si5351 LO for the Tayloe detector, I/Q out over USB UAC */
  BOOT_MODE_MK52 = 3,     /* Elektronika MK-52 calculator */
} boot_mode_t;

/* Decide the startup mode: shows "MacJr <version>" and the mode list on
 * `display`, then blocks until a menu key is pressed. Returns
 * BOOT_MODE_TTY_DEMO if the keyboard is unavailable. Call after
 * keyboard_init(). */
boot_mode_t boot_select_mode(macjr_hal_display_t *display);

#ifdef __cplusplus
}
#endif

#endif
