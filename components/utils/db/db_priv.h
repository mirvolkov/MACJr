#ifndef MACJR_DB_PRIV_H
#define MACJR_DB_PRIV_H

#include "db.h"

/* Register DB_LISTEN_DEV (db_listen.c). Called by db_init(). */
esp_err_t db_listen_init(void);

/* Tell the listeners whose prefix matches that `key` changed. */
void db_listen_notify(const char *key);

#endif
