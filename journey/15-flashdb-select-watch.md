# 15. Waiting without polling

Programs used to poll: REMOTE asked the IR receiver for a command every
20 ms and then peeked at the keyboard in non-blocking mode. That works
for one source. It doesn't work for a program that wants to hear about
keys, settings changes and, one day, other programs, all at once. Unix
answers this with `select()`, and ESP-IDF's VFS supports it, as long as
each driver does its part. This step gave the keyboard and the config
store that part, and on the way swapped the config store's engine.

## FlashDB instead of our text file

The text store from story 12 rewrote the whole file for every update.
We looked for something small and sturdy to replace it. SQLite had
already been too heavy. FlashDB was a good fit: a log-structured
key-value store written for microcontrollers, with a CRC on every record
and garbage collection. It has a "file mode" that treats ordinary files
as flash sectors, so it runs on the SD card unchanged.

The `db` component now keeps four 4 KB sector files,
`/sdcard/DB/config.fdb.0` to `.3`, behind the same CRUD API. FlashDB
2.2.0 is vendored unmodified under `components/hal/db/flashdb`. It added
about 7 KB of flash and 1 KB of RAM.

Two things needed care:

- **Syncing.** FlashDB keeps its sector files open and only calls
  `fflush()`, but FAT commits data on `fsync()` or `fclose()`. So every
  `db_*` call ends by syncing FlashDB's cached file handles. After that,
  a power cut can't lose what the call wrote.
- **Migration.** On first start, `db_init()` imports the old
  `config.db` once. Keys the database already has are kept. The old file
  is then renamed to `config.old`.

## select() on the keyboard

A VFS driver supports `select()` through two callbacks:

- `start_select` gets the caller's descriptor sets and a semaphore. The
  driver clears the sets and remembers what was asked for.
- `end_select` forgets the call.

When a key arrives, the keyboard task adds the descriptor back to the
caller's read set and calls `esp_vfs_select_triggered()`, which wakes
the caller. If keys are already queued when `select()` starts, the
driver triggers right away.

REMOTE now waits in `select()` on `/dev/kbrd` with a 20 ms timeout, then
checks the IR receiver. IR isn't a file yet, so the timeout stays for
now. Once it becomes `/dev/ir`, that timeout can go too.

## Watching the database

The same device pattern gives the config store notifications:

```c
int w = db_watch("radio.");      /* opens /dev/db/radio. */
/* ... select() on w together with the keyboard ... */
read(w, buf, sizeof buf);        /* "radio.freq\n" */
```

Every successful create, update or delete of a key starting with the
watch's prefix queues one `key\n` line on that watch. Each watch has its
own 256-byte queue. If a slow reader lets its queue overflow, the queue
is replaced by a single `*\n` line, meaning "changes were lost, read
everything again". Nothing more is added until that line has been read.

## We considered interrupts too

We also asked whether programs could get signals as software interrupts.
On the ESP32-S3 this is a poor fit. Interrupt handlers can do very
little, and a program's code lives in PSRAM, which can't be reached
while the flash cache is off. Signals, when they come, will be a
pending-bit mask that the system checks at safe points, as Unix does.
For now, descriptors and `select()` cover the need.

## A little rain

PLASMA got a second mode: `-r` for the old rainbow noise, `-m` for a
matrix-style rain. In the rain, each column has a falling drop with a
white head, a bright-green cell behind it and a green trail that
flickers. The boot menu passes no arguments, so matrix is the default.
The options are checked by their first two characters.

**Result:** a sturdier config store, a keyboard and a database that
work with `select()`, and a pattern any new device can copy: a queue
per `open()` plus the two select callbacks.
