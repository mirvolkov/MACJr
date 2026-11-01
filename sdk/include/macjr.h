/*
 * MacJr API for programs loaded from the SD card (see components/exec).
 * The firmware exports these functions to the ELF loader by name; a
 * program calls them like ordinary functions and the loader binds the
 * calls when it loads the program.
 *
 * A program's entry point gets the screen as a third argument:
 *
 *   int main(int argc, char *argv[], macjr_tty_t *tty);
 *
 * The screen is a 40x15 text console with ANSI escapes and CP866 glyphs.
 * Draw on it with the tty API (tty/tty.h): macjr_tty_write(),
 * macjr_tty_header(), macjr_tty_winsize() and macjr_display_flush() --
 * output shows up on the panel after a flush. snprintf()/vsnprintf()
 * format text for it.
 *
 * A program runs in its own task, normally on CPU1 at idle priority, until
 * main() returns or it calls sys_exit(); the firmware keeps running its
 * own tasks (keyboard, sound, monitor) meanwhile and always comes first.
 * While the system monitor (Cx) is open it owns the screen: sys_getkey()
 * waits until it closes, so a program that draws after a key press never
 * paints over it.
 *
 * Besides this API a program may use the C library functions and the
 * firmware's HAL calls the loader exports (components/exec/exec.c lists
 * them); printf() goes to the serial console, not to the screen. Not
 * available: thread-local data, errno included (picolibc keeps it per
 * task, and the loader can't relocate that).
 */
#ifndef MACJR_H
#define MACJR_H

#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The program's entry point, as the loader calls it. */
typedef int (*macjr_main_t)(int argc, char *argv[], macjr_tty_t *tty);

/* Block until a key is pressed and return its code (KEY_* in the
 * firmware's keyboard.h; MK-52 keys are 128-159). */
int sys_getkey(void);

#define SYS_KEY_ENTER 148 /* MK-52 В↑ */

/* Milliseconds since boot. */
unsigned sys_millis(void);

/* Sleep for `ms` milliseconds. */
void sys_delay(unsigned ms);

/* The CPU core the program runs on (0 or 1). */
int sys_cpu(void);

/* End the program now with exit code `status`, as if main() returned it. */
void sys_exit(int status) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif
