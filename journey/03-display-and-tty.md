# 3. The display and the tty

Everything the user sees goes through a 320x240 ST7789 panel and a 40x15
text console. Most of what we learned about it came from screens that looked
slightly wrong.

## The boot menu and two display bugs

With several modes wanting the same board, a `boot` component draws a start
screen and waits for a key. The title scales the 8x16 font by 3/2 to 12x24 on
the fly, and the version comes from `PROJECT_VER` in CMake.

The first screen showed the title twice. The driver collects drawing in a
40-row buffer, but it always sent full 320-pixel rows. The TTY redraws whole
rows, so it never noticed. The menu draws part of a row, and the rest went
out with stale pixels. We now track which pixels were really drawn and send
only those.

Next, the green title came out purple. The panel reads the high byte of a
pixel first, the ESP32 stores the low byte first, and nothing swapped them.
Green `0x57EA` reached the panel as `0xEA57`. An old "BGR" setting had hidden
this for pure red. One swap fixed every colour:

```c
uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
return (uint16_t)((c << 8) | (c >> 8));
```

The menu now lists built-in programs first, in cyan, then `/BIN/*.ELF`,
sorted, with `_` shown as a space. It holds ten entries, the tenth on key 0.

## Drawing while sending

The calculator's 64-pixel digits exposed a latent bug: a call taller than the
40-row buffer would have written past its end, so tall calls are now split
into bands.

PLASMA, which redraws the whole text area every frame, showed the screen
being painted from top to bottom. The driver drew a strip, sent it, and
*waited*, so the CPU and the SPI bus took turns. Now there are two strip
buffers. `lcd_blit_async()` queues a strip and returns a ticket, drawing goes
on in the other buffer, and `lcd_wait(ticket)` guards reuse. Strips finish in
order, so a counter in the "transfer done" interrupt is all the bookkeeping
needed. A frame takes about as long as the longer job, not the sum. Some
sweep is left: only syncing to the panel's TE pin would hide it completely.

PLASMA began as a port of an ncurses effect that took half a CPU on the Mac,
so it became random glyphs in random colours (`-r`). Its default mode, `-m`,
is a matrix rain: a falling drop per column, with a white head and a
flickering green trail.

## The system monitor

We wanted one chord that shows what the machine is doing and then puts the
screen back. A `monitor` task sleeps on `/dev/kbrd` until F + K arrives, then:

1. Saves the screen with `macjr_tty_save_screen()`: all 600 cells plus the
   cursor and colours, about 2.4 KB.
2. Draws the page with ordinary ANSI escapes, so it needs no drawing code.
3. Refreshes every second until the chord comes again, then restores.

```
 MacJr system monitor
────────────────────────────────────────
CPU0 ████░░░░░░░░░░░░░░░░  18%
CPU1 █░░░░░░░░░░░░░░░░░░░   3%

Memory
SRAM     123 /   340 KB used
PSRAM     96 /  8192 KB used

Temp   41.5 C
```

CPU load comes from how long each core's idle task ran, memory from the heap
totals, temperature from the chip's own sensor, and the SD card from
`sdcard_info()`. Bars turn yellow above 50 % and red above 80 %. Two modes
needed help: the USB drive pauses its status lines on `monitor_is_open()`, and
the PNG viewer gets a redraw callback, because its picture is not in the text
cells.

## A window that scrolls

Networks don't fit a 40x15 screen for long, so the tty got its first real
widget: a list window. The app doesn't hand it the items. The list asks for
the text of one item by index as it draws, so it can be longer than the
window, and the same code will serve file lists later.

The bar started as `░` and `█` on the left, moved to the right edge, and was
then redrawn after a picture of the DOS original: arrows at both ends, a
checkered track and a one-cell thumb. That took work on the font too:

- `▒` was vertical stripes, not a checker.
- `■` filled the cell's full width and needed margins.
- CP866 has no arrows, so `▲` and `▼` went into codes 30 and 31, where the
  classic DOS font keeps them. The console used to drop those control codes.

## Box corners nobody noticed

The first useful [screenshot](07-pictures.md) came from IDE DEMO, and zooming
in showed something invisible on the small panel: in `╔ ╗ ╚ ╝` the horizontal
lines crossed the verticals, and the inner line stopped short. Each corner is
really two nested L shapes. Redrawn on the same rows as `═` (5 and 9) and the
same columns as `║` (1-2 and 5-6), they join their neighbours exactly:

```
.######################.
.##..................##.
.##..##############..##.
.##..##..........##..##.
```

**Result:** a double-buffered driver, a console with colours, a CP866 font
with arrows and correct boxes, a list window, and a monitor that works on top
of any screen.
