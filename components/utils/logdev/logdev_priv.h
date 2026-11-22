#ifndef MACJR_LOGDEV_PRIV_H
#define MACJR_LOGDEV_PRIV_H

#include <stdbool.h>
#include <stdint.h>

#include "logdev.h"

/* The files on the card (log_file.c). Only the log's flush lock holder,
 * or log_init(), calls these. */

/* LOG_DIR is there (made if needed): the card can be written. */
bool log_file_start(void);

/* The newest record number in the files, 0 if none. */
uint32_t log_file_last(void);

/* Call `cb` for each record in the files numbered `from` or more. */
typedef void (*log_file_cb_t)(const log_rec_t *rec, void *ctx);
void log_file_load(uint32_t from, log_file_cb_t cb, void *ctx);

/* Append `n` records (oldest first) to their programs' files: the number
 * of the newest one saved, 0 if they couldn't be. */
uint32_t log_file_append(const log_rec_t *recs, int n);

#endif
