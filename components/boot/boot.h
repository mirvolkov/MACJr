#ifndef MACJR_BOOT_H
#define MACJR_BOOT_H

#include <stddef.h>

#include "macjr.h"
#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A program built into the firmware (development: no SD card round trip). */
typedef struct {
  const char *name; /* as shown in the navigator */
  macjr_main_t main;
} boot_builtin_t;

/* The boot shell: a file navigator over the SD card in the style of DOS
 * Navigator, on `tty`. It starts in the card's root, where the
 * `n_builtins` built-in programs are listed first, and blocks until В↑
 * picks a program. A built-in one goes to `*builtin`; for a *.ELF file,
 * or a *.PY script to run, `*builtin` is NULL and its full path goes to
 * `path`. The directory and
 * the selection are kept for the next call, so the navigator comes back
 * where it was when the program ends. Call after keyboard_init(). */
void boot_select(macjr_tty_t *tty, const boot_builtin_t *builtins, int n_builtins, char *path, size_t path_len,
                 const boot_builtin_t **builtin);

#ifdef __cplusplus
}
#endif

#endif
