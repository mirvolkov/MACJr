# 6. The system monitor

Every OS needs a way to look inside itself. We wanted one key that, at any
moment, shows what the machine is doing and then puts the screen back
exactly as it was.

```
 MacJr system monitor
────────────────────────────────────────
CPU0 ████░░░░░░░░░░░░░░░░  18%
CPU1 █░░░░░░░░░░░░░░░░░░░   3%

Memory
SRAM     123 /   340 KB used
PSRAM     96 /  8192 KB used

Temp   41.5 C

SD     mounted at /sdcard
       14800 / 15200 MB free

 Press key 140 to close
```

## How it works

A `monitor` task opens `/dev/kbrd` like any other reader and sleeps until
the monitor key arrives. Then it does three things:

1. **Saves the screen.** We added `macjr_tty_save_screen()` and
   `macjr_tty_restore_screen()` to the TTY. They copy all 600 character
   cells plus the cursor and colours, about 2.4 KB.
2. **Draws the page** with ordinary ANSI escape codes, so the monitor
   needs no drawing code of its own:
   ```c
   line(3, "CPU%d %s %3d%%", core, bar, pct);   /* "\x1b[3;1H" ... "\x1b[K" */
   ```
3. **Refreshes every second** until the key comes again, then restores
   the saved screen.

## Where the numbers come from

- **CPU load:** FreeRTOS run-time stats record how long each core's idle
  task ran. The load is the part of the last second the core was *not*
  idle.
- **Memory:** the heap reports totals for internal SRAM and PSRAM.
- **Temperature:** the ESP32-S3's built-in sensor (the chip, not the room).
- **SD card:** a new `sdcard_info()` returns the size and free space of the
  mounted card.

The build failed twice on the way: `tty.c` did not include its own public
header, so the new type was unknown, and the compiler refused an empty
format string (`line(13, "")`). Both were one-line fixes.

**Result:** an overlay that works on top of any screen. The bars turn
yellow above 50 % and red above 80 %.
