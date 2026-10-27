#ifndef MACJR_BOOT_H
#define MACJR_BOOT_H

#include "tty/hal_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hold this key while powering up to get the boot menu. */
#define BOOT_KEY 158

/* Raw key codes of the "1" / "2" / "3" keys that pick menu entries. Key 3
 * has no menu entry yet. */
#define BOOT_MENU_KEY_1 134
#define BOOT_MENU_KEY_2 133
#define BOOT_MENU_KEY_3 139

typedef enum {
  BOOT_MODE_TTY_DEMO = 1, /* default: TTY + Turbo C IDE demo screen */
  BOOT_MODE_SDR_UAC = 2,  /* Si5351 LO for the Tayloe detector, I/Q out over USB UAC */
} boot_mode_t;

/* Decide the startup mode. Without BOOT_KEY held this returns
 * BOOT_MODE_TTY_DEMO immediately and draws nothing. With it held, shows
 * "MacJr <version>" and the mode list on `display`, then blocks until a
 * menu key is pressed. Call after keyboard_init(). */
boot_mode_t boot_select_mode(macjr_hal_display_t *display);

#ifdef __cplusplus
}
#endif

#endif
