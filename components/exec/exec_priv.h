#ifndef MACJR_EXEC_PRIV_H
#define MACJR_EXEC_PRIV_H

#include <stdbool.h>

/* The next key for a read() of /dev/tty (ttydev.c): a code as
 * sys_getkey() gives it, waiting for one if `block`; -EAGAIN if none is
 * waiting and not `block`, -EPERM outside the program's task. */
int exec_tty_key(bool block);

/* Free the hardware the program left taken (exec_hw.c). */
void exec_hw_release(void);

#endif
