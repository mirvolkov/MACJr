#ifndef MACJR_TTYDEV_H
#define MACJR_TTYDEV_H

#include "esp_err.h"
#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * /dev/tty: the running program's terminal as a character device (see
 * ttydev.c). write() prints through the tty and flushes the screen;
 * read() gives the keys, a key code per byte. A program also has it as
 * descriptors 0, 1 and 2 (stdin, stdout, stderr), and printf(), puts(),
 * putchar() print there (exec.c).
 */
#define TTY_DEV "/dev/tty"

/* st_rdev of its descriptors: how exec tells them from other devices */
#define TTY_RDEV 0x7474

/* Register TTY_DEV and keep descriptors 0..2 from going to files. At
 * boot (main.c); exec calls it again before a program, which does nothing
 * once it is up. */
esp_err_t ttydev_init(void);

/* The tty TTY_DEV reaches: the program's while it runs, NULL after. */
void ttydev_attach(macjr_tty_t *tty);

#ifdef __cplusplus
}
#endif

#endif
