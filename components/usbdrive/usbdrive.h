#ifndef MACJR_USBDRIVE_H
#define MACJR_USBDRIVE_H

#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Show the SD card's root directory on `tty`, then unmount it and expose
 * the card to the USB host as a drive, updating the connection and
 * transfer status on screen. Never returns: the card stays with the host
 * until the next reboot. */
void usbdrive_run(macjr_tty_t *tty);

#ifdef __cplusplus
}
#endif

#endif
