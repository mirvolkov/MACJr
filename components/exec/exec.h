#ifndef MACJR_EXEC_H
#define MACJR_EXEC_H

#include "esp_err.h"
#include "sdcard.h"
#include "macjr.h"
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
#define EXEC_STACK_SIZE 16384 /* internal SRAM: MicroPython runs in 16 KB, as on its ESP32 port */

/* Exit code of a killed program: 128 + SIGKILL (9), as Unix shells show
 * it. */
#define EXEC_KILLED 137

/* exec_run() in a new task on EXEC_CORE; the caller blocks until the
 * program ends. F and Cx pressed together kill it meanwhile (exit code
 * EXEC_KILLED; see exec.c). Also ESP_ERR_NO_MEM if the task can't be
 * created. One program at a time. */
esp_err_t exec_spawn(macjr_tty_t *tty, const char *path, int argc, char **argv, int *status);

/* Run the program at `path` with exec_spawn() (argv[0] = its file name)
 * on a cleared screen, then show its exit code or why it couldn't run and
 * wait for Cx. A killed program returns at once, without a message. */
void exec_launch(macjr_tty_t *tty, const char *path);

/* exec_launch() with one argument: argv[1] = `arg`. How the navigator
 * opens a file with the program for it (a .py script with UPYTHON.ELF). */
void exec_launch_with(macjr_tty_t *tty, const char *path, const char *arg);

/* exec_launch() for a program built into the firmware: `entry` is its
 * main(), `name` its argv[0]. For development: no SD card round trip. */
void exec_launch_main(macjr_tty_t *tty, const char *name, macjr_main_t entry);

#ifdef __cplusplus
}
#endif

#endif
