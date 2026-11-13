#ifndef MACJR_MONITOR_H
#define MACJR_MONITOR_H

#include <stdbool.h>

#include "esp_err.h"
#include "keyboard.h"
#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Key that opens / closes the monitor: MK-52 F and K pressed together
 * (a chord, see keyboard.h). */
#define MONITOR_KEY KEY_MK_F_K

/* Key that saves a screenshot: MK-52 F and ↑↓ pressed together. Works at
 * any time, on any screen, the monitor's too. */
#define SCREENSHOT_KEY KEY_MK_F_UPDOWN

/* Key that turns USB keyboard mode on or off: MK-52 F and А↑ together.
 * On, the USB OTG port is a host and a USB keyboard plugged into it types
 * into /dev/kbrd alongside the MK-52 keys (usb_kbd_start, usbdev.h). */
#define USB_KBD_KEY KEY_MK_F_A_UP

/* Key that turns Wi-Fi on or off: MK-52 F and БП together. A notice window
 * shows the new state. The handler is supplied by the firmware via
 * monitor_set_wifi_cb (the radio/db policy lives there, not here). */
#define WIFI_KEY KEY_MK_F_BP

/* Start the monitor task. From then on MONITOR_KEY, pressed at any time,
 * saves the screen of `tty` and shows CPU load per core, internal RAM and
 * PSRAM usage, chip temperature and SD card status, refreshed every second.
 * Pressing it again restores the saved screen. Call after keyboard_init()
 * and once `tty` exists. */
esp_err_t monitor_start(macjr_tty_t *tty);

/* Save what the panel shows as SHOTS/SHOTnnnn.PNG on the SD card. True on
 * success, with the file ("SHOTS/SHOT0008.PNG") in `msg`; false with the
 * reason in `msg`. SCREENSHOT_KEY does this, and shows the outcome in a
 * window for a second. */
bool screenshot_save(macjr_tty_t *tty, char *msg, size_t len);

/* Switch MONITOR_KEY handling off (for an app that needs that key itself)
 * or back on. On by default. */
void monitor_set_enabled(bool on);

/* True while the monitor is on screen. Apps that keep updating the tty
 * must skip drawing then, or they would paint over the monitor. */
bool monitor_is_open(void);

/* Called (from the monitor task) right after the monitor closes and the
 * tty screen is restored. Apps use it to repaint anything they drew
 * outside the tty, or that changed while the monitor was open. */
typedef void (*monitor_redraw_cb_t)(void *ctx);
void monitor_set_redraw_cb(monitor_redraw_cb_t cb, void *ctx);

/* Toggle Wi-Fi: called (from the monitor task) when WIFI_KEY is pressed.
 * Returns the new state (true = Wi-Fi now on). The monitor shows a notice
 * with the result. Without a callback set, WIFI_KEY does nothing. */
typedef bool (*monitor_wifi_cb_t)(void);
void monitor_set_wifi_cb(monitor_wifi_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif
