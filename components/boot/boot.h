#ifndef MACJR_BOOT_H
#define MACJR_BOOT_H

#include <stddef.h>

#include "macjr.h"
#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Programs the menu shows, picked with the MK-52 digit keys 1-9, then 0. */
#define BOOT_MAX_ENTRIES 10

/* A program built into the firmware (development: no SD card round trip). */
typedef struct {
  const char *name; /* as shown in the menu */
  macjr_main_t main;
} boot_builtin_t;

/* Show "MacJr <version>", the `n_builtins` built-in programs and the
 * *.ELF files of EXEC_BIN_DIR by name on `tty`, and block until a digit
 * key picks one. A built-in one goes to `*builtin`; for a file
 * `*builtin` is NULL and its full path goes to `path`. With no programs
 * at all (no card, empty /BIN) it says so and looks again on every key.
 * Call after keyboard_init(). */
void boot_select(macjr_tty_t *tty, const boot_builtin_t *builtins, int n_builtins, char *path, size_t path_len,
                 const boot_builtin_t **builtin);

#ifdef __cplusplus
}
#endif

#endif
