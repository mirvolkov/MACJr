#ifndef MACJR_MONITOR_H
#define MACJR_MONITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Start the monitor task. `tty` is the programs' tty, `own` a second one
 * on the same display that only the monitor draws on. From then on
 * MONITOR_KEY, pressed at any time, brings `own` to the front with CPU
 * load per core, internal RAM and PSRAM usage, chip temperature and SD
 * card status, refreshed every second; pressing it again brings `tty`
 * back (macjr_tty_set_front). Programs keep writing to `tty` meanwhile.
 * The notice windows (screenshot, USB keyboard, Wi-Fi) show on `own` too,
 * over a copy of the screen. Call after keyboard_init() and once both
 * ttys exist. */
esp_err_t monitor_start(macjr_tty_t *tty, macjr_tty_t *own);

/* A screenshot in two steps, so that something can be shown on screen
 * between them without ending up in the picture. screenshot_grab() copies
 * what the panel shows (fast: from the screen's copy in PSRAM) and returns
 * the RGB888 image, or NULL with the reason in `msg`. screenshot_write()
 * saves it as SHOTS/SHOTnnnn.PNG on the SD card (slow: PNG encoding and
 * the card) and frees it; true with the file ("SHOTS/SHOT0008.PNG") in
 * `msg`, false with the reason. SCREENSHOT_KEY does both, showing a
 * "Taking screenshot" window meanwhile and then the outcome for a second. */
uint8_t *screenshot_grab(macjr_tty_t *tty, char *msg, size_t len);
bool screenshot_write(macjr_tty_t *tty, uint8_t *rgb, char *msg, size_t len);

/* Switch MONITOR_KEY handling off (for an app that needs that key itself)
 * or back on. On by default. */
void monitor_set_enabled(bool on);

/* True while the monitor (or one of its notices) is on screen. The keys
 * are the monitor's then: programs reading /dev/kbrd themselves ignore
 * them. Drawing needs no care, it goes to the program's tty in the
 * background. A program that draws on the display HAL itself (the image
 * viewer) repaints when this goes back to false. */
bool monitor_is_open(void);

/* Toggle Wi-Fi: called (from the monitor task) when WIFI_KEY is pressed.
 * Returns the new state (true = Wi-Fi now on). The monitor shows a notice
 * with the result. Without a callback set, WIFI_KEY does nothing. */
typedef bool (*monitor_wifi_cb_t)(void);
void monitor_set_wifi_cb(monitor_wifi_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif
