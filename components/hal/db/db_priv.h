#ifndef MACJR_DB_PRIV_H
#define MACJR_DB_PRIV_H

#include "db.h"

/* Register DB_WATCH_DEV (db_watch.c). Called by db_init(). */
esp_err_t db_watch_init(void);

/* Tell the watches whose prefix matches that `key` changed. */
void db_watch_notify(const char *key);

#endif
