/*
 * FlashDB configuration (flashdb/inc/flashdb.h includes it by this name).
 * Databases live in files on the SD card (libc FILE API, through the FAT
 * VFS), not in raw flash: no FAL.
 */
#ifndef _FDB_CFG_H_
#define _FDB_CFG_H_

/* Key-value databases (the db component) */
#define FDB_USING_KVDB

/* Time series databases: compiled in for logs later; unused code is
 * dropped by the linker */
#define FDB_USING_TSDB

/* Sector files via fopen/fread/fwrite/fclose */
#define FDB_USING_FILE_LIBC_MODE

/* assert() instead of FlashDB's own, which spins in while (1) */
#define FDB_USING_NATIVE_ASSERT

#endif /* _FDB_CFG_H_ */
