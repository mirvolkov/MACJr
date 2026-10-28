#ifndef MACJR_MK52_H
#define MACJR_MK52_H

#include "tty/hal_display.h"
#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Run the Elektronika MK-52 calculator on `tty`, reading the MK-52 keys
 * from /dev/kbrd. The digit line is drawn at double size straight onto
 * `display` (the panel behind `tty`). Never returns; runs in the calling
 * task, which needs an 8 KB stack (float formatting). */
void mk52_run(macjr_tty_t *tty, macjr_hal_display_t *display);

#ifdef __cplusplus
}
#endif

#endif
