# 18. Taking a picture of the screen

The list window from [story 16](16-wifi.md) needed a proper scroll bar,
and getting one led to a closer look at the font. Sending pictures of
the screen around showed how much a screenshot would help, so the board
learned to take its own.

## A scroll bar from the DOS days

A throwaway LIST DEMO program put a long list on screen so the scrolling
could be judged by eye. The bar made of `░` and `█` worked but looked
wrong, so it was redrawn after a picture of the real thing: arrows at
both ends, a checkered track and a one-cell thumb that follows the
selection.

That took more than new code in the list:

- `▒` was vertical stripes, not a checker.
- `■` filled the cell's full width and needed margins.
- CP866 has no arrows, so `▲` and `▼` went into codes 30 and 31, where
  the classic DOS font keeps them. The console used to drop those control
  codes, and now prints them.

LIST DEMO did its job and was removed. While it was there it pushed the
boot menu to ten entries, one more than digits 1-9 can pick, and the last
program would have silently vanished. So `0` is the tenth entry now.

## There is no framebuffer

F and ↑↓ pressed together save the screen, but what is "the screen"?
Pixels go straight to the panel through small row buffers, and the ST7789
can't be read back over this bus. Rendering the text cells would have been
easy, but PORT draws its picture behind the tty, so a screenshot of it
would come out empty.

So the display HAL keeps a shadow: 150 KB in PSRAM, updated with every
span it sends to the panel. Whatever reaches the glass is in that copy,
in the panel's byte-swapped RGB565.

## Writing a PNG without a compressor

The `png` component could only read. A PNG doesn't have to be compressed,
though: deflate has "stored" blocks, which are just a length and the raw
bytes. So `png_write_file()` writes one block per row, each in its own
IDAT chunk:

```
signature, IHDR, IDAT x 240 (one row each), IDAT (Adler-32), IEND
```

It needs no compressor and no extra buffer, and the file is about 230 KB.
The ROM has a real compressor that could shrink text screens a lot, but it
can't be tried on the Mac, and this writer could: Python's zlib read a test
image back byte for byte, with every CRC correct, and macOS opened it too.

The monitor task handles the key, since it already listens at all times.
It copies the image first and then writes it, so the running program
carries on. Files go to `SHOTS/SHOT0001.PNG` and on.

PLASMA exits on any key, so the new chord would have ended it. Chords
belong to the system now, and `KBD_IS_CHORD()` lets programs skip them.

## The first thing a screenshot showed

The first useful picture came from IDE DEMO, and zooming in showed
something nobody had noticed on the small panel: the double-line corners
were wrong. In `╔ ╗ ╚ ╝` the horizontal lines crossed the verticals, and
the inner line stopped short.

Each corner is really two nested L shapes. Redrawn on the same rows as `═`
(5 and 9) and the same columns as `║` (1-2 and 5-6), they join their
neighbours exactly:

```
.######################.
.##..................##.
.##..##############..##.
.##..##..........##..##.
```

**Result:** a DOS-style scroll bar, a correct box frame, and screenshots
of anything on the panel with one key combination. Saving gives no signal
on the device yet: the outcome only goes to the log.
