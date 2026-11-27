#ifndef MACJR_EXEC_EXPORTS_H
#define MACJR_EXEC_EXPORTS_H

#include <stdarg.h>
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
ssize_t exec_write(int fd, const void *buf, size_t len);
int exec_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
void exec_vTaskDelay(TickType_t ticks);
int exec_usleep(useconds_t us);
unsigned exec_sleep(unsigned s);
void exec_exit(int status) __attribute__((noreturn));

/* stdio for a program: to its descriptor 1, the tty (ttydev.h) */
int exec_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int exec_vprintf(const char *fmt, va_list ap);
int exec_puts(const char *s);
int exec_putchar(int c);

/* sdcard_open_raw() for a program: closes the db first, and the card comes
 * back (filesystem, db) when the program ends, however it ends. */
esp_err_t exec_sdcard_open_raw(sdmmc_card_t **out);

/* Hardware a program takes (exec_hw.c): the same calls, noting the ADC
 * units, calibrations and PWM channels, which are freed or stopped when
 * the program ends, however it ends. */
#include "driver/ledc.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
esp_err_t exec_adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *ret);
esp_err_t exec_adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle);
esp_err_t exec_adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg,
                                                    adc_cali_handle_t *ret);
esp_err_t exec_ledc_channel_config(const ledc_channel_config_t *cfg);

#endif
