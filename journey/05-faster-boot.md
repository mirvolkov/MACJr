# 5. Faster boot

A calculator should be ready the moment it is switched on, so we went
through the ESP-IDF settings that slow down start-up.

## What we changed

| Setting | Before | After | Why |
|---|---|---|---|
| PSRAM test at boot | on | off | It writes test patterns over all 8 MB |
| Flash mode | DIO | QIO | Reads flash four bits at a time instead of two |
| CPU clock | 160 MHz | 240 MHz | The chip's maximum, about 1.5x faster |
| Image check on deep-sleep wake | on | off | Only matters for deep sleep, which we don't use yet |

Two lessons came out of this. First, `sdkconfig` still says
`FLASHMODE="dio"` after choosing QIO, which looks like a mistake but is
not: the chip's built-in boot code starts the flash in DIO, and our
bootloader switches it to quad mode later. Second, the deep-sleep option
does nothing for a normal power-on, so we noted the setting that would
(`SKIP_VALIDATE_ON_POWER_ON`) but left it off, because it also stops a
corrupted app from being caught at boot.

## The PSRAM test we took back out

Turning the boot test off left a question: can we still test PSRAM when
we want to? ESP-IDF has a function for it, but it overwrites all of PSRAM,
which would destroy live data once the app runs. So we wrote our own: grab
the largest free PSRAM block, run four patterns over it, and free it.

```c
/* fixed patterns catch stuck data lines, address patterns catch
 * address-line faults: every word gets a value unique to its address */
0x55555555, 0xAAAAAAAA, address, ~address
```

We put it on boot menu key 3, and the board reset itself with:

```
W (47) boot.esp32s3: PRO CPU has been reset by WDT.
W (52) boot.esp32s3: APP CPU has been reset by WDT.
```

Those lines are only the bootloader reporting the previous crash; the real
cause was not captured. Rather than guess, we rolled the test back and
kept the faster boot.

**Result:** a quicker start at 240 MHz with QIO flash, and a note for
later: catch the full crash log before trying the PSRAM test again.
