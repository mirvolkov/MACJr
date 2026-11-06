# 17. Kill, and leave no mess

Until now a program ended when it chose to. Some never chose to: the
radio and the USB drive ran until power off. This step gave the keyboard
an escape key and key combinations, and gave the system a way to end any
program and put everything back.

## ESC and chords

Cx became ESC: the keyboard sends ASCII 27, and every program treats it
as "back". The exception is the calculator, where Cx is still "clear X",
because it can't work without it.

Could the keyboard read two keys at once? It always could. A scan drives
one row and reads every column, so two keys held together both show up.
The matrix has no diodes, so three keys on the corners of a rectangle
would show a phantom fourth, but a pair is always read correctly.

The hard part of a chord is the first key. F has already been pressed by
the time K arrives, and sending it at once would leave a stray F in the
program. So keys that can start a chord wait: the partner turns the pair
into one code, a release lets the key through alone, and any other key
sends it first. F and K together open the monitor, and Cx stopped doing
that job. F and Cx together kill the program.

## Asking first

A task can be deleted at any moment, but it might be inside `select()`,
registered with a driver, or holding a lock. So a kill asks first.

Programs don't call `read()` or `select()` directly any more. The loader
checks its own libc table first, but it lets the firmware replace its
symbol lookup, so these names now lead to wrappers in `exec`. Each one
waits in 50 ms slices and checks for a kill in between. The program leaves
through `sys_exit()`, on its own stack, like any normal ending.

The first version looped forever on files: ESP-IDF's `select()` skips
drivers that don't support it and simply times out. Only character devices
take the slicing path now; files don't block anyway.

If a program is still computing a second later, its task is deleted where
it stands. A program blocked inside a firmware call, such as a Wi-Fi
connect, is waited for instead, because deleting it could leave a lock
taken for good.

The wrappers track open descriptors too, so any program's leftovers are
closed when it ends. A killed program goes straight back to the boot menu,
with exit code 137 in the log, as on Unix: 128 + SIGKILL.

## The card that didn't come back

Killing USBDRIVE worked, and then nothing could read the SD card. The
program had unmounted FAT and handed the card to USB, and nothing undid
that, because it was never meant to end.

There was a worse problem underneath. FlashDB keeps its files open between
calls. Across an unmount those `FILE` handles go stale, and their
descriptor numbers can later belong to some other file. So the db now
closes before the card goes away and reopens after it returns. A test on
the Mac ran 50 close and reopen cycles, with no value lost and no file left
open.

## USB back to where it started

Detaching the drive wasn't enough: USB stayed in the mode it had started
in, so USBDRIVE after a killed RADIO would fail until reboot. So
`usbdev_stop()` now takes USB all the way back to its default:

1. Detach from the host, and let any transfer finish.
2. Let the TinyUSB task leave its loop, then shut the stack down.
3. Give the native port back to USB-Serial-JTAG.

Step 3 needed a closer look. `usb_del_phy()` leaves the internal PHY
routed to USB OTG, and what routes it back is creating a PHY for
USB-Serial-JTAG. The UAC component has no shutdown at all, so its mic task
is put to sleep the way the host does it, by closing the stream, and the
next start wakes it.

The order matters: USB goes down first and the card comes back last, so
the host and the ESP32 can never write to it at the same time. Eject the
drive before killing USBDRIVE, though. To the host a kill is a pulled
cable.

**Result:** F and Cx end any program, and what it took comes back: its
descriptors, its PSRAM, USB and the SD card. The USB teardown had not been
tried on the board when this was written. The Si5351 keeps running after
the radio ends, since its driver has no stop call yet.
