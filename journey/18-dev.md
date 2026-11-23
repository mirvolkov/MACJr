# 18. /dev: the terminal as a file

A program used to get its screen only as the third argument of `main()`
and wrote to it with `macjr_tty_write()`. `printf()` worked, but its text
went to the serial console, which nobody looks at on a calculator. Now the
screen is also a file, `/dev/tty`, and `printf()` prints on it. The devices
got a directory of their own too, so `/dev` can be listed like any other
folder.

## /dev/tty

The device has no tty of its own. While a program runs, exec attaches that
program's tty; when it ends, it detaches it. Every descriptor of `/dev/tty`
reaches the screen of the program that is running, the way `/dev/tty` on
Unix reaches a process's controlling terminal. Between programs `open()`
fails with `ENXIO`.

- `write()` goes through the tty: ANSI escapes, CP866 glyphs, and `\n`
  becoming `\r\n`. The screen is flushed after each write, so text shows up
  at once. The flush redraws only the rows that changed, so it is cheap.
- `read()` gives the keys, one key code per byte, the same codes
  `sys_getkey()` returns. It blocks, or fails with `EAGAIN` under
  `O_NONBLOCK`, and F + Cx still kills a program waiting in it.

## stdout, the hard way

The natural plan was to point each program's `stdout` at its tty. ESP-IDF
6 uses picolibc, though, and picolibc has a single global `stdout` for all
tasks, so there is no per-task stream to redirect. Instead the firmware
exports its own `printf()`, `vprintf()`, `puts()` and `putchar()` to
programs. The loader looks in our table before its libc table, so a
program's `printf()` binds to ours. Ours formats into a buffer and writes
to descriptor 1.

Descriptors 0, 1 and 2 of a program are one `/dev/tty` descriptor that exec
opens for it: `write(1, ...)` prints, `read(0, ...)` gives keys, and
`close()` of them does nothing. That only works if the VFS never hands
those numbers to a file. picolibc opens just two console descriptors, so
fd 2 could have been free and a program's `open()` could have got it. At
boot the firmware takes whatever is still free below 3 with a console
descriptor.

## Eight slots

Counting before flashing turned up a bug. IDF's VFS holds
`CONFIG_VFS_MAX_COUNT` = 8 entries. IDF takes four: `/dev/null`,
`/dev/uart`, `/dev/secondary` (the USB-JTAG port as the second console) and
`/dev/console`. We had four more: the card, `/dev/kbrd`, `/dev/db` and
`/dev/log`. The table was full, so `/dev/tty` would have failed to register
at boot, and `printf()` would quietly have gone back to the serial console.
`/dev` itself needs one more slot. The limit is 16 now, in
`sdkconfig.defaults` too.

## A directory for the devices

Each device is a VFS entry of its own, and nobody owned `/dev` itself:
`opendir("/dev")` failed, and the navigator only ever browsed the card.
The VFS sends a path to the entry with the longest matching prefix, so a
new component, **devfs**, registers `/dev` as a directory and only lists.
`/dev/log` still goes straight to the log driver.

A driver calls `devfs_add("log", false)` after it has registered, so a
device that failed to start isn't listed. IDF's own devices still open,
but they stay out of the listing: `/dev` shows the board's devices, not
the plumbing.

```
═════════════════[/DEV]═════════════════
║..
║db
║kbrd
║log
║tty
```

The navigator shows it as `DEV` in the card's root, in blue after the
built-ins. Devices are listed by name only. В↑ doesn't open one: the text
viewer on `/dev/kbrd` would just wait for keys. The file menu has nothing
to delete, copy or paste there. The listing reads `d_type` from `readdir()`
instead of calling `stat()`, because most drivers have no `stat()` for
their own path.

The db's change notification became `db_listen()` along the way
([story 8](08-config-db.md)). `/dev/db` is the namespace of its listeners,
`/dev/db/<prefix>`, so it is listed as a directory.

**Result:** the firmware builds with `/dev/tty`, `printf()` on the screen,
descriptors 0 to 2 and a listable `/dev`. The host compiler builds
`SRC/lines.c`, and every app on the card passes the export check. Still
open: none of it has run on the board yet, and `select()` on `/dev/tty`
isn't supported.
