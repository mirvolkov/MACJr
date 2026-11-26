/*
 * Updates over Wi-Fi: a small HTTP server, up while Wi-Fi is connected,
 * that takes a new firmware and the programs (tools/deploy.py sends them).
 *
 *   GET    /info          firmware, slot, and the programs on flash
 *                         (name, size, CRC-32) as JSON
 *   PUT    /ota           a build/MACJr.bin: written to the other app slot,
 *                         which boots next (ota.h); restarts unless
 *                         ?reboot=0
 *   PUT    /bin/name.elf  a program, into /bin (EXEC_BIN_DIR: the apps partition);
 *                         names are lower case
 *   DELETE /bin/name.elf
 *   POST   /reboot
 *
 * If the db has NETUPD_DB_KEY_TOKEN, every request must carry it in an
 * X-Token header; without it anyone on the network can update the board.
 */
#ifndef MACJR_NETUPD_H
#define MACJR_NETUPD_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETUPD_PORT         80
#define NETUPD_DB_KEY_TOKEN "net.token"

/* Start the server (if it isn't running). Call once Wi-Fi is connected. */
esp_err_t netupd_start(void);

/* Stop it (if running), before Wi-Fi goes down. */
void netupd_stop(void);

/* True once after a restart this service made (a firmware update or
 * /reboot): the board should rejoin Wi-Fi by itself, so the update can
 * go on (the programs after the firmware), and confirm a new firmware
 * only once the service is up again. */
bool netupd_rejoin(void);

#ifdef __cplusplus
}
#endif

#endif
