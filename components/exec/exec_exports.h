#ifndef MACJR_EXEC_EXPORTS_H
#define MACJR_EXEC_EXPORTS_H

#include <stddef.h>
#include <sys/select.h>
#include <sys/types.h>

#include "esp_elf.h"
#include "freertos/FreeRTOS.h"
#include "sdcard.h"

/* Firmware functions programs may call (exports.c). The loader looks
 * them up here before its own libc table, so an entry here wins. */
extern const struct esp_elfsym g_exec_exports[];

/* Export `fn` to programs under the name `name`. */
#define EXEC_EXPORT_AS(name, fn) {#name, (void *)(fn)}

/* What programs get for these calls (exec.c): the same, plus they keep
 * track of the program's descriptors, which are closed when it ends, and
 * the blocking ones end the program when it is killed. */
int exec_open(const char *path, int flags, int mode);
int exec_close(int fd);
ssize_t exec_read(int fd, void *buf, size_t len);
int exec_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
void exec_vTaskDelay(TickType_t ticks);
int exec_usleep(useconds_t us);
unsigned exec_sleep(unsigned s);
void exec_exit(int status) __attribute__((noreturn));

/* sdcard_open_raw() for a program: closes the db first, and the card comes
 * back (filesystem, db) when the program ends, however it ends. */
esp_err_t exec_sdcard_open_raw(sdmmc_card_t **out);

#endif
