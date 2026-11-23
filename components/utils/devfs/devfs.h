#ifndef MACJR_DEVFS_H
#define MACJR_DEVFS_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The /dev directory. Each device is a VFS entry of its own (/dev/log,
 * /dev/tty, ...), and the VFS sends a path to the entry with the longest
 * matching prefix, so DEVFS_DIR itself belongs to nobody: opendir("/dev")
 * fails. devfs registers DEVFS_DIR as a directory that lists the devices:
 * opendir()/readdir() give one entry per device, d_type DT_CHR (DT_DIR
 * for a device with names under it, as /dev/db/<key>), and stat("/dev")
 * says it is a directory. Opening a device still goes straight to its own
 * driver; devfs only lists.
 *
 * A device is listed once its driver calls devfs_add() after registering
 * it, so a device that failed to start isn't there. Only the firmware's
 * own devices are listed; ESP-IDF's (/dev/null, /dev/console, /dev/uart,
 * ...) still open but stay out of the listing.
 */
#define DEVFS_DIR "/dev"

#define DEVFS_MAX      16 /* devices listed */
#define DEVFS_NAME_MAX 16 /* a name, terminator included */

/* List device `name` (no slashes: "log" for /dev/log); `is_dir` for a
 * device with names under it. Registers DEVFS_DIR on the first call. */
esp_err_t devfs_add(const char *name, bool is_dir);

#ifdef __cplusplus
}
#endif

#endif
