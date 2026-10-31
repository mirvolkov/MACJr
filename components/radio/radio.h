#ifndef MACJR_RADIO_H
#define MACJR_RADIO_H

#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the SDR local oscillator and stream I/Q to the USB host as a UAC
 * microphone, then show the active frequency on `tty` and retune it from
 * the keyboard: digits and '.' type a new frequency in MHz, ←ШГ deletes
 * the last character, В↑ makes it active and saves it in config.db for
 * the next start. Never returns. */
void radio_run(macjr_tty_t *tty);

#ifdef __cplusplus
}
#endif

#endif
