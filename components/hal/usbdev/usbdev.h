#ifndef MACJR_USBDEV_H
#define MACJR_USBDEV_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What the host sees on the USB OTG port. Fixed at usbdev_start(). */
typedef enum {
  USBDEV_MODE_UAC, /* 2-channel UAC 2.0 microphone (see mic_uac_start) */
  USBDEV_MODE_MSC, /* the SD card as a USB drive */
} usbdev_mode_t;

/* Interface number of the UAC microphone stream, for
 * uac_device_config_t.mic_itf_num. */
#define USBDEV_ITF_MIC 1

/* Bring up the USB PHY and TinyUSB in `mode`. Takes the native USB port
 * from USB-Serial-JTAG (flash and monitor through the other USB-C port
 * then). For USBDEV_MODE_MSC call usbdev_msc_set_card() first.
 * ESP_ERR_INVALID_STATE if USB is already up. */
esp_err_t usbdev_start(usbdev_mode_t mode);

/* Back to the default (device modes only): detach from the host, stop TinyUSB and give the
 * native USB port to USB-Serial-JTAG again, as after reset. Any mode can
 * start afterwards. A USB drive lets go of its card first, once a transfer
 * under way has ended; the host sees the drive pulled out, so what it
 * hadn't written yet is lost. Does nothing if USB isn't up. */
void usbdev_stop(void);

/* USB keyboard: the OTG port as a USB host (TinyUSB HID), so a USB
 * keyboard plugged into it types into /dev/kbrd (keyboard_inject(); the
 * mapping is in usb_kbd.c). The keyboard needs 5 V on VBUS, which the port
 * doesn't supply by itself. ESP_ERR_INVALID_STATE while the port is in use
 * (a USB drive or microphone). usbdev_stop() leaves it alone. */
esp_err_t usb_kbd_start(void);

/* Back to USB-Serial-JTAG, as usbdev_stop() is for the device modes. */
void usb_kbd_stop(void);

bool usb_kbd_active(void);    /* host mode is up */
bool usb_kbd_connected(void); /* and a keyboard is plugged in */

/* The card the USB drive exposes, opened raw (sdcard_open_raw). */
void usbdev_msc_set_card(sdmmc_card_t *card);

typedef struct {
  bool mounted;           /* host has configured the device */
  bool ejected;           /* host ejected the drive */
  uint64_t bytes_read;    /* by the host, since start */
  uint64_t bytes_written;
} usbdev_msc_status_t;

void usbdev_msc_status(usbdev_msc_status_t *out);

#ifdef __cplusplus
}
#endif

#endif
