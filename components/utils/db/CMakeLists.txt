# ESP32-S3 HAL backend: key-value config store on the SD card, a FlashDB
# key-value database in file mode, and /dev/db change watches. Component
# name (derived from this directory) is "db".
#
# flashdb/: FlashDB 2.2.0 (https://github.com/armink/FlashDB, tag 2.2.0,
# 65a8764), Apache-2.0 (flashdb/LICENSE), sources unmodified; configured
# by fdb_cfg.h here.
idf_component_register(
  SRCS "db.c" "db_watch.c"
       "flashdb/src/fdb.c"
       "flashdb/src/fdb_file.c"
       "flashdb/src/fdb_kvdb.c"
       "flashdb/src/fdb_tsdb.c"
       "flashdb/src/fdb_utils.c"
  INCLUDE_DIRS "." "flashdb/inc"
  REQUIRES sdcard freertos vfs
)
