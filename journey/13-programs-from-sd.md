# 13. Programs from the SD card

Until now every mode of the board was part of the firmware: the
calculator, the radio, the USB drive, the picture viewer. Each new app
meant a new firmware and a new flash. We wanted what DOS had: programs as
files on a disk, started from a menu, one at a time.

## ELF files and a loader

Espressif's `elf_loader` component loads a position-independent ELF file
at run time. It copies the program's sections into memory and fixes the
addresses inside them. Then it binds each function the program calls to
the firmware's own copy, by name. We load into PSRAM: the S3 can run code
from there through its instruction-bus alias at `0x42...`, while the data
sits at `0x3C...`.

A program is a C file with a `main` that also gets the screen:

```c
int main(int argc, char *argv[], macjr_tty_t *tty);
```

An early version wrapped the screen in calls like `sys_write()`. We
dropped them. A program draws with the same tty calls the firmware uses
(`macjr_tty_write`, `macjr_tty_header`, `macjr_display_flush`), and only
a few `sys_*` calls are left: keys, time, delay, exit. `sys_exit()` jumps
back to the loader with `longjmp`.

The firmware lists every function a program may call in
`components/exec/exports.c`. The loader adds a small libc table of its
own (`malloc`, `memcpy`, `printf`, ...). Anything missing fails at load
time with "Can't find symbol".

## The second core

The firmware's own tasks run on CPU0, so a program gets CPU1. It runs in
a task of its own there, with a 12 KB stack and priority 0, the same as
the idle task. They share the core in turns, so a busy program still
lets the idle task run, and the watchdog stays quiet. The loader waits
for the task to finish and then shows the program's exit code.

## Building without IDF

Apps are not IDF components. `apps/Makefile` builds every `apps/<name>/`
directory into `<NAME>.ELF`. It takes the firmware's include paths and
defines from `build/compile_commands.json`, so a program sees the same
headers the firmware was built with. Compiling and linking are separate
steps, because the picolibc specs file made the shared-object link crash
`ld`.

Three things the loader rejects, each found on the board the hard way:

- **errno.** picolibc keeps it per task as thread-local data, and the
  loader can't apply that relocation. Programs simply don't use errno.
- **`.eh_frame`.** A 64-bit division (`__udivdi3`) pulled an unwind
  table from libgcc into the port viewer. The table was dropped at link
  time, but a leftover `R_XTENSA_NONE` relocation remained, and the loader
  answered "Invalid ELF image -22". The Makefile now links against a copy
  of libgcc with `.eh_frame` removed.
- **Stale binaries.** Twice the "bug" was an old file on the card.

`apps/tools/checkelf.py` now checks the same rules at build time:
imports against the export table, relocation types, and sections. A bad
program fails `make`, not the board.

## Everything moves out

With the loader working, the firmware shrank to the HALs, the tty, the
boot menu, the system monitor and the loader. The calculator, radio, USB
drive and port viewer moved to `apps/`, with two new programs: `HELLO`
and `IDE DEMO` (the old TTY demo screen). Along the way, all apps became
console apps that draw only through the tty. The PNG viewer is the one
agreed exception. App headers now look like
`════ TITLE ════`, and the MK-52's digit line is plain red text.

The boot menu lists `/BIN/*.ELF`, sorted, with `_` shown as a space. It
skips the `._NAME.ELF` files macOS leaves on FAT, and `make install` also
removes them with `dot_clean`.

The last new program, `PLASMA`, started as a port of an ncurses plasma
effect. On the Mac it took half a CPU, so we kept it simple instead: the
screen fills with random glyphs in random colours, frame after frame.
Watching it showed the display drawing from top to bottom, which led to
the double buffering in [story 4](04-boot-menu.md).

**Result:** a firmware that boots to a menu of programs on the SD card,
and apps that are rebuilt and copied with `make install`, without
reflashing.
