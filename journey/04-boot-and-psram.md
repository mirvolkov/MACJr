# 4. Faster boot and the PSRAM hang

A calculator should be ready the moment it is switched on, so we tuned the
ESP-IDF start-up. That tuning uncovered the hardest bug of the project.

## What we changed

| Setting | Before | After | Why |
|---|---|---|---|
| PSRAM test at boot | on | off | It writes test patterns over all 8 MB |
| Flash mode | DIO | QIO | Reads flash four bits at a time instead of two |
| CPU clock | 160 MHz | 240 MHz | The chip's maximum |
| Image check on deep-sleep wake | on | off | We don't use deep sleep yet |

`sdkconfig` still says `FLASHMODE="dio"` after choosing QIO, which is not a
mistake: the ROM starts the flash in DIO and our bootloader switches later.
We left `SKIP_VALIDATE_ON_POWER_ON` off, because it would stop a corrupted app
from being caught at boot.

## A crash that hid from us

Shortly after the calculator arrived, the board started resetting itself:

```
rst:0x8 (TG1WDT_SYS_RST)
Saved PC:0x40375cd8  panic_enable_cache at panic_handler.c:277
W (47) boot.esp32s3: PRO CPU has been reset by WDT.
```

No backtrace: something crashed, the crash handler got stuck, and the
interrupt watchdog reset the chip. Each usual suspect was wrong: a small
stack, QIO flash, a stack overflow (a hardware watchpoint never fired), the
calculator code (it crashed in a mode that runs none). Adding a few log lines
made it vanish, and a bug that hides when code moves around is a memory bug
or a race. An earlier PSRAM self-test on a menu key had died the same way, and
we had rolled it back without an answer.

## Bringing in the debugger

The S3's USB port includes JTAG. We attached OpenOCD and GDB to the exact
crashing firmware, with hardware breakpoints ahead of the stuck cache code:

```
hbreak panicHandler
hbreak xt_unhandled_exception
```

CPU0 turned out to be spinning on `0x3c060000`, the start of PSRAM, and the
heap's internals showed why:

```
heap_t 0x3fc9b034: start 0x3c060000 ... mux owner 0xcdcd count 1
```

CPU0 itself held the PSRAM heap's lock. It had entered the allocator, switched
interrupts off, and never came out, most likely looping in corrupted
free-list data. The watchdog fired, and the handler on CPU1 could not stop
CPU0, so it hung as well.

Experiments showed that only the boot-time PSRAM test kept it away, at either
clock speed. We turned the test back on, knowing it might just be masking the
problem.

## The real cause

It was. Starting the [USB drive](06-usb.md) brought the resets back, and this
time the trail led to the wiring: the LCD reset line was on GPIO35, and on the
N16R8 module GPIO33-37 are the octal PSRAM's data lines. Once `lcd_init()`
drove that pin, PSRAM accesses were corrupted, which is exactly what the
broken heap lists looked like. The boot test had only hidden it by touching
PSRAM before the LCD started.

```c
#define ST7789_RST 14  // Reset (was GPIO35, an octal-PSRAM data line)
```

**Result:** a quick start at 240 MHz with QIO flash, and 8 MB of PSRAM in
octal mode at 80 MHz with no boot test. GPIO33-37 stay free from now on. The
lesson: when prints make a bug disappear, stop printing and attach a debugger.
