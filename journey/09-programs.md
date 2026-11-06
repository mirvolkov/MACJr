# 9. Programs from the SD card

At first every mode of the board was part of the firmware, and each new app
meant a new flash. We wanted what DOS had: programs as files on a disk,
started from a menu, one at a time. Then we wanted them to end cleanly too.

## ELF files and a loader

Espressif's `elf_loader` copies a position-independent ELF into memory, fixes
its addresses, and binds each function it calls to the firmware's copy by
name. We load into PSRAM: the S3 runs code from there through its
instruction-bus alias at `0x42...`, while the data sits at `0x3C...`.

```c
int main(int argc, char *argv[], macjr_tty_t *tty);
```

A program draws with the same tty calls the firmware uses, and only a few
`sys_*` calls are left: keys, time, delay, exit. `sys_exit()` jumps back to the
loader with `longjmp`. Every function a program may call is listed in
`components/exec/exports.c`, and anything missing fails with "Can't find
symbol".

The firmware's tasks run on CPU0, so a program gets CPU1, in a task with a
12 KB stack at priority 0, the same as the idle task. They share the core in
turns, so a busy program doesn't upset the watchdog.

## Building without IDF

`apps/Makefile` builds every `apps/<name>/` into `<NAME>.ELF`, taking include
paths and defines from `build/compile_commands.json`. Three things the loader
rejects were found on the board the hard way:

- **errno.** picolibc keeps it as thread-local data, and the loader can't apply
  that relocation. Programs simply don't use errno.
- **`.eh_frame`.** A 64-bit division pulled an unwind table from libgcc, and a
  leftover `R_XTENSA_NONE` relocation gave "Invalid ELF image -22". The Makefile
  now links against a libgcc with `.eh_frame` removed.
- **Stale binaries.** Twice the "bug" was an old file on the card.

`apps/tools/checkelf.py` now checks imports, relocation types and sections at
build time, so a bad program fails `make`, not the board.

## Everything moves out

The firmware shrank to the HALs, the tty, the boot menu, the monitor and the
loader. All apps became console apps that draw only through the tty, with the
PNG viewer as the one agreed exception. The menu skips the `._NAME.ELF` files
macOS leaves on FAT, and `make install` removes them with `dot_clean`.

New apps now start as built-ins, because copying an ELF on every change is
slow. `exec_launch_main()` runs them exactly like a program from `/BIN`. They
call only what an SD-card program can, so they move to `apps/` as they are.

## Killing a program

Some programs never chose to end: RADIO and USBDRIVE ran until power off. A task
can be deleted at any moment, but it might be inside `select()` or holding a
lock. So a kill asks first.

Programs no longer call `read()` or `select()` directly. The firmware replaces
the loader's symbol lookup, so these names lead to wrappers in `exec` that wait
in 50 ms slices and check for a kill. The program leaves through `sys_exit()`,
like any normal ending. The first version looped forever on files, because
`select()` skips drivers that don't support it, so only character devices take
the slicing path.

If a program is still computing a second later, its task is deleted where it
stands. One blocked inside a firmware call, such as a Wi-Fi connect, is waited
for instead, because deleting it could leave a lock taken for good.

The wrappers track open descriptors too, so leftovers are closed when a program
ends. A killed program goes back to the boot menu with exit code 137, as on
Unix: 128 + SIGKILL. The [card](08-config-db.md) and [USB](06-usb.md) needed
their own work to come back.

We also asked whether signals could be software interrupts. That is a poor fit:
a program's code lives in PSRAM, which can't be reached while the flash cache is
off. Signals, when they come, will be a pending-bit mask checked at safe points,
as Unix does.

**Result:** a firmware that boots to a menu of programs on the SD card, apps
rebuilt and copied with `make install` without reflashing, and F + Cx to end any
of them and get back what it took.
