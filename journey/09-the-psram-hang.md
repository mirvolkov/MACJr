# 9. Catching a crash that hid from us

Shortly after the calculator arrived, the board started resetting itself.
The log showed only the aftermath:

```
rst:0x8 (TG1WDT_SYS_RST)
Saved PC:0x40375cd8  panic_enable_cache at panic_handler.c:277
W (47) boot.esp32s3: PRO CPU has been reset by WDT.
```

No error message, no backtrace. Something crashed, then the crash handler
itself got stuck, and the interrupt watchdog reset the chip.

## The usual suspects, one by one

We went down the list and each guess was wrong:

- **A too-small stack?** We moved the calculator into its own task, then
  back, with more stack. Still crashing.
- **QIO flash mode?** Back to DIO. Still crashing.
- **A stack overflow?** A hardware watchpoint on every task's stack end
  never fired.
- **The calculator code?** It crashed in mode 1, where no calculator code
  runs at all. Stashing the calculator made the crash go away, though.

Then the strangest part: adding a few log lines made it vanish, even
without any delays. A bug that hides when the code moves around in memory
is a memory bug or a race, and adding prints was never going to find it.

## Bringing in the debugger

The ESP32-S3's USB port includes a JTAG interface, and it works next to
the serial console. We attached OpenOCD and GDB to the exact crashing
firmware and set hardware breakpoints on the crash handler's two entry
points, so GDB would stop before the stuck cache code:

```
hbreak panicHandler
hbreak xt_unhandled_exception
```

The first capture showed CPU1 in the interrupt watchdog handler, and the
`main` task on CPU0 inside `malloc` with what looked like a corrupt stack.
The second capture dumped the registers: CPU0 was not corrupted, it was
spinning. The address it worked on was `0x3c060000`, the start of PSRAM.
The third capture read the heap's internals at the moment of the hang:

```
heap_t 0x3fc9b034: start 0x3c060000 ... mux owner 0xcdcd count 1
```

The PSRAM heap's lock was held by CPU0 itself. So CPU0 had entered the
PSRAM allocator, taken its lock (which switches interrupts off), and never
came back out, most likely looping in corrupted free-list data. With
interrupts off the watchdog fired, and the crash handler on CPU1 could not
stop CPU0, so it hung as well. That was the whole picture.

## What we know, and what we don't

None of our code uses PSRAM: the memory there belongs to ESP-IDF drivers,
which get PSRAM first under `CONFIG_SPIRAM_USE_MALLOC`. Experiments on the
board then showed:

| Setting | Result |
|---|---|
| Boot-time PSRAM test off, 240 MHz | crashes |
| Boot-time PSRAM test off, 160 MHz | crashes |
| Boot-time PSRAM test on, 240 MHz | runs fine |

**Result:** the PSRAM test is back on and the CPU stays at 240 MHz. The
root cause is still open. The test writes all 8 MB at boot, which may only
be masking the problem. The next step, when needed, is to keep driver
memory out of PSRAM with `CONFIG_SPIRAM_USE_CAPS_ALLOC`. The lesson stays
either way: when prints make a bug disappear, stop printing and attach a
debugger.

## The real cause, found later

Story 10 brought the crash back and finally explained it: the LCD reset
line was wired to GPIO35, and on the N16R8 module GPIO33-37 are the octal
PSRAM's data lines. Once `lcd_init()` drove that pin, PSRAM reads and
writes were corrupted, which is exactly what the broken heap lists looked
like. The boot test had only hidden it by touching PSRAM before the LCD
started.

The reset wire moved to GPIO14, a free pin, and the code followed:

```c
#define ST7789_RST 14  // Reset (was GPIO35, an octal-PSRAM data line)
```

**Final result:** PSRAM is on again, 8 MB in octal mode at 80 MHz, with no
boot-time test, and the crashes are gone. GPIO33-37 stay free from now on.
