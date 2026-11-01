#ifndef MACJR_BOOT_H
#define MACJR_BOOT_H

#include <stddef.h>

#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Programs the menu shows, picked with the MK-52 digit keys 1-9. */
#define BOOT_MAX_ENTRIES 9

/* Show "MacJr <version>" and the *.ELF files of EXEC_BIN_DIR by name on
 * `tty`, and block until a digit key picks one; its full path goes to
 * `path`. With no programs (no card, empty /BIN) it says so and looks
 * again on every key. Call after keyboard_init(). */
void boot_select(macjr_tty_t *tty, char *path, size_t path_len);

#ifdef __cplusplus
}
#endif

#endif
