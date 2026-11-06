# 7. Pictures: PNG in and out

The board needed to show its own port layout, so PORT displays
`IMG/layout.png` from the SD card, scaled to fit. Later the same `png`
component learned to write, which gave us screenshots.

## A decoder without a library

PSRAM was off at the time, leaving about 300 KB of RAM, and a 320x240 picture
alone would take 230 KB. The image had to stream through memory.

After an 8-byte signature, a PNG is chunks: `IHDR`, `PLTE`, and `IDAT` chunks
that together hold one zlib stream. The hard part is already in the chip: the
ROM contains miniz's `tinfl_decompress`. We feed it 1 KB at a time and it
writes into a 32 KB window that wraps around:

```c
tinfl_decompress(inflator, in, &in_size, window, window + ofs, &out_size,
                 TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
```

Out come scanlines, each with a filter byte (Sub, Up, Average, Paeth). Undo it
against the previous row, convert to RGB, hand the row to the caller, and only
two rows are ever kept. The decoder needs about 45 KB, whatever the picture
size.

Scaling streams too. The display gained `blit_rgb888`, and screen row `ty`
shows source row `ty * h / th`, so each arriving row is drawn zero, one or
several times.

We tested it on the Mac, with a stand-in mapping `tinfl_decompress` onto zlib
and Pillow as the reference:

```
rgb8       (97, 61) max diff 0 OK
pal_trns   (97, 61) max diff 0 OK
big.png    1200x800, 21 IDAT chunks, max diff 0
```

Ten formats came out pixel-exact. One test lied: the "interlaced" file was not
interlaced, because Pillow quietly ignores that option. Setting the flag by
hand confirmed the decoder refuses such files cleanly.

## There is no framebuffer

F and ↑↓ together save the screen, but what is "the screen"? Pixels go straight
to the panel through small row buffers, and the ST7789 can't be read back over
this bus. Rendering the text cells would miss PORT's picture. So the display
HAL keeps a shadow: 150 KB in PSRAM, updated with every span it sends. Whatever
reaches the glass is in that copy, in the panel's byte-swapped RGB565.

## Writing a PNG without a compressor

Deflate has "stored" blocks, which are just a length and the raw bytes. So
`png_write_file()` writes one block per row, each in its own IDAT chunk:

```
signature, IHDR, IDAT x 240 (one row each), IDAT (Adler-32), IEND
```

It needs no compressor and no extra buffer, and the file is about 230 KB. The
ROM's real compressor could shrink text screens a lot, but it can't be tried
on the Mac, and this writer could: Python's zlib read a test image back byte
for byte, with every CRC correct.

The monitor task handles the chord, since it always listens. It copies the
image first and then writes it to `SHOTS/SHOT0001.PNG` and on, so the running
program carries on.

**Result:** any PNG up to 2048 pixels wide on screen in a few seconds, and
screenshots of anything on the panel, which soon paid off by
[exposing bad box corners](03-display-and-tty.md). Saving gives no signal on
the device yet: the outcome only goes to the log.
