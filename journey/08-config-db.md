# 8. The config database

The calculator and future apps need somewhere to keep settings between
power-ups. The SD card is the obvious place; the format took three tries.

## SQLite, then a text file

The first attempt put SQLite on the board. It built, but it brought a large
library for a handful of strings, so we rolled it back.

The second was our own `key=value` text file behind a plain CRUD API:

```c
db_create("volume", "800");          /* ESP_ERR_NOT_ALLOWED if it exists */
db_read("volume", buf, sizeof buf);  /* ESP_ERR_NOT_FOUND, ..._INVALID_SIZE */
db_update("volume", "400");
db_delete("volume");
```

Every update copied the file to `config.tmp` and swapped it in, with a recovery
step at boot, because FAT can't rename over an existing file. Testing on the
Mac with stubbed ESP-IDF headers caught a bug in Cyrillic keys: `getc()` returns
0-255, but `char` is signed there, so a CP866 byte like `0x8C` compared as -116.
One cast fixed it.

## FlashDB

Rewriting the whole file for every update was clumsy. FlashDB fitted well: a
log-structured key-value store for microcontrollers, with a CRC on every record
and garbage collection. Its "file mode" treats ordinary files as flash sectors,
so it runs on the SD card unchanged.

The `db` component now keeps four 4 KB sector files, `/sdcard/DB/config.fdb.0`
to `.3`, behind the same API. FlashDB 2.2.0 is vendored unmodified under
`components/hal/db/flashdb`, for about 7 KB of flash and 1 KB of RAM.

- **Syncing.** FlashDB only calls `fflush()`, but FAT commits on `fsync()` or
  `fclose()`. So every `db_*` call ends by syncing FlashDB's cached file
  handles, and a power cut can't lose what the call wrote.
- **Migration.** On first start `db_init()` imports the old `config.db` once,
  keeping keys the database already has, and renames it `config.old`.

## Watching the database

The [keyboard's device pattern](01-keyboard.md) gives the store notifications:

```c
int w = db_watch("radio.");      /* opens /dev/db/radio. */
/* ... select() on w together with the keyboard ... */
read(w, buf, sizeof buf);        /* "radio.freq\n" */
```

Every successful change of a key with that prefix queues one `key\n` line. Each
watch has its own 256-byte queue. If a slow reader overflows it, the queue is
replaced by a single `*\n` line, meaning "changes were lost, read everything
again".

## Surviving an unmount

FlashDB keeps its files open between calls. When USBDRIVE unmounts the card,
those `FILE` handles go stale, and their descriptor numbers can later belong to
some other file. So the db now closes before the card goes away and reopens
after it returns. A Mac test ran 50 cycles with no value lost and no file left
open.

**Result:** a sturdy config store that programs can wait on with `select()`,
and that survives both power cuts and the card being taken away.
