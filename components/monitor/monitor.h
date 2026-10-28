#ifndef MACJR_MONITOR_H
#define MACJR_MONITOR_H

#include "esp_err.h"
#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Raw key code that opens / closes the monitor. */
#define MONITOR_KEY 140

/* Start the monitor task. From then on MONITOR_KEY, pressed at any time,
 * saves the screen of `tty` and shows CPU load per core, internal RAM and
 * PSRAM usage, chip temperature and SD card status, refreshed every second.
 * Pressing it again restores the saved screen. Call after keyboard_init()
 * and once `tty` exists. */
esp_err_t monitor_start(macjr_tty_t *tty);

#ifdef __cplusplus
}
#endif

#endif
