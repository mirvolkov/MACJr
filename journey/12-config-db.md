# 12. A config file of our own

The calculator and future apps need somewhere to keep settings between
power-ups. The SD card is the obvious place; the question was the format.

## SQLite, tried and dropped

The first attempt put SQLite on the board: one `config.sqlite` file with
a single key-value table behind a small CRUD API. It built, but it
brought a large library for what is, for now, a handful of strings. We
rolled it back. A database written from scratch can come later, as part
of the OS itself.

## One pair per line

The `db` component keeps `/sdcard/config.db` as plain text, for example:

```
# lines without '=' are ignored and kept
volume=800
mk52_angle=DEG
```

A key is everything before the first `=` and the value everything after
it, with no trimming. Lines may end in LF, CR or CRLF, so the file can be
edited on any computer. The API is plain CRUD on strings:

```c
db_create("volume", "800");          /* ESP_ERR_NOT_ALLOWED if it exists */
db_read("volume", buf, sizeof buf);  /* ESP_ERR_NOT_FOUND, ..._INVALID_SIZE */
db_update("volume", "400");
db_delete("volume");
```

Nothing is cached and no file stays open. Every call opens the file, scans
it a character at a time and closes it again, so memory use doesn't grow
with the file, and the card can be handed to USB (story 10) at any moment.
A mutex makes the calls safe from any task.

## Rewriting without losing the file

Reading and creating only scan and append. Updating and deleting have to
change a line in the middle, so the file is copied to `config.tmp` with
that one line changed, flushed to the card, and then swapped in. FAT
can't rename over an existing file, so the swap takes two steps:

```c
unlink(DB_PATH);
rename(DB_TMP, DB_PATH);
```

If power fails between them, only `config.tmp` is left, and `db_init()`
renames it back at the next boot. If both files are there, the rewrite
never finished, so the original is kept and the copy deleted.

## A bug in Cyrillic

We tested the component on the Mac first, with stubs for the ESP-IDF
headers. Keys in Cyrillic (CP866, which the font now covers) were never found:
`getc()` returns bytes as 0-255, but `char` on the Mac is signed, so a key
byte like `0x8C` compared as -116. One cast fixed it, and the tests now
pass with both signed and unsigned `char`:

```c
match = match && k < key_len && (unsigned char)key[k] == c;
```

**Result:** `db_init()` runs at boot right after the SD card mounts and
creates `config.db` if it is missing. Settings can now be stored, read
back and edited by hand on a computer.
