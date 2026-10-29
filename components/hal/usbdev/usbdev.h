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
 * then). For USBDEV_MODE_MSC call usbdev_msc_set_card() first. Call once. */
esp_err_t usbdev_start(usbdev_mode_t mode);

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
