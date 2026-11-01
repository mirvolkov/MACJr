#ifndef MACJR_EXEC_H
#define MACJR_EXEC_H

#include "esp_err.h"
#include "sdcard.h"
#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Programs live here on the SD card. */
#define EXEC_BIN_DIR SDCARD_MOUNT_POINT "/BIN"

/* Load the ELF program at `path` into PSRAM and run its
 * main(argc, argv, tty) in the calling task (sdk/include/macjr.h). Returns once the program ends; its exit code goes to `*status`.
 * Errors: ESP_ERR_NOT_FOUND (no file), ESP_ERR_NO_MEM, ESP_ERR_INVALID_ARG
 * (not a loadable ELF, or it needs a symbol the firmware doesn't export). */
esp_err_t exec_run(macjr_tty_t *tty, const char *path, int argc, char **argv, int *status);

/* Program task for exec_spawn(): CPU1, so a busy program leaves CPU0 to
 * the system; idle priority, so it shares CPU1 round-robin with the idle
 * task (which keeps the task watchdog fed) and every system task preempts
 * it. */
#define EXEC_CORE       1
#define EXEC_PRIORITY   0
#define EXEC_STACK_SIZE 12288 /* the MK-52 formats floats */

/* exec_run() in a new task on EXEC_CORE; the caller blocks until the
 * program ends. Also ESP_ERR_NO_MEM if the task can't be created. One
 * program at a time. */
esp_err_t exec_spawn(macjr_tty_t *tty, const char *path, int argc, char **argv, int *status);

/* Run the program at `path` with exec_spawn() (argv[0] = its file name)
 * on a cleared screen, then show its exit code or why it couldn't run and
 * wait for В↑. */
void exec_launch(macjr_tty_t *tty, const char *path);

#ifdef __cplusplus
}
#endif

#endif
