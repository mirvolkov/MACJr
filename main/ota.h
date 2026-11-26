/*
 * Firmware updates: two app slots (partitions.csv), the firmware runs from
 * one and installs an update into the other.
 *
 * A new firmware boots on trial. ota_confirm() keeps it; any reset before
 * that (a crash, a hang and a power cycle) makes the bootloader go back to
 * the previous one.
 */
#ifndef MACJR_OTA_H
#define MACJR_OTA_H

#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The update file on the SD card. Once installed it is renamed to
 * OTA_CARD_DIR/MACJR-<version>.BIN, which keeps the firmwares that were
 * installed (copy one back to MACJR.BIN to go back to it). */
#define OTA_CARD_DIR  "/sdcard/UPDATE"
#define OTA_CARD_FILE OTA_CARD_DIR "/MACJR.BIN"

/* Logs the slot and firmware that run. With В/О held at power-up, boots
 * the firmware in the other slot instead (if there is one): the way back
 * from a firmware that starts but misbehaves. Call after keyboard_init(). */
void ota_boot(void);

/* Installs OTA_CARD_FILE if it holds a MacJr firmware other than the one
 * that runs, showing the progress on tty, and restarts into it. Returns
 * if there is nothing to install or the install failed (said on tty). */
void ota_from_card(macjr_tty_t *tty);

/* The firmware is up: keep it (no rollback any more). Call once the board
 * works -- display, keyboard, SD card and the card updater have run. */
void ota_confirm(void);

#ifdef __cplusplus
}
#endif

#endif
