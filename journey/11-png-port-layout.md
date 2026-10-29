# 11. A picture from the SD card

The board needed a way to show its own port layout, a picture of which
connector is which. Boot menu option 5 does that: it shows
`IMG/layout.png` from the SD card, scaled to fit the screen.

## A PNG decoder without a library

With PSRAM off there are about 300 KB of RAM, and a 320x240 picture alone
would take 230 KB as 24-bit pixels. The image had to stream through
memory, never sitting in it whole.

PNG is simpler than it looks. After an 8-byte signature come chunks:
`IHDR` with the size and pixel format, `PLTE` with a palette, and `IDAT`
chunks that together hold one zlib stream. The hard part, inflating zlib,
is already in the ESP32-S3: its ROM contains miniz's `tinfl_decompress`.
We feed it 1 KB of file at a time and it writes into a 32 KB window that
wraps around:

```c
tinfl_decompress(inflator, in, &in_size, window, window + ofs, &out_size,
                 TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
```

What comes out are scanlines, each starting with a filter byte. The
filter says how the row was stored: as differences against the pixel to
the left (Sub), the row above (Up), their average, or the Paeth guess. Undo
it against the previous row, convert the pixels to RGB, hand the row to
the caller, and only two rows are ever kept. The whole decoder needs about
45 KB, whatever the picture size.

## Testing it on the Mac

To test the decoder before it touched the board, we compiled it on the
Mac. A tiny stand-in mapped `tinfl_decompress` onto the Mac's zlib, and
Python's Pillow wrote PNGs in every format and decoded them as the
reference:

```
rgb8       (97, 61) max diff 0 OK
pal4       (97, 61) max diff 0 OK
gray16     (97, 61) max diff 0 OK
pal_trns   (97, 61) max diff 0 OK
...
big.png    1200x800, 21 IDAT chunks, max diff 0
```

Ten formats, palettes, transparency and a 1.4 MB picture all came out
pixel-exact. One test lied, though: the "interlaced" file was not
interlaced, because Pillow quietly ignores that option. Setting the flag
by hand confirmed the decoder refuses such files cleanly.

## Drawing and scaling

The display interface could only draw solid rectangles and font glyphs, so
it gained `blit_rgb888` for true-color rows. Scaling streams too: screen
row `ty` shows source row `ty * h / th`. As rows arrive in order, each one
is drawn zero times when shrinking, once, or several times when enlarging,
so the full image is never needed.

## The monitor, everywhere

The system monitor (key Cx) now works in every mode except the
calculator, which needs Cx itself. Two modes needed help. The USB drive
keeps updating its status lines, so it now checks `monitor_is_open()` and
pauses. The picture is drawn outside the text screen, so restoring the
screen would wipe it. A redraw callback, `monitor_set_redraw_cb()`, now
lets the picture be decoded again once the monitor closes.

**Result:** option 5 shows the port layout in a few seconds, any PNG
size up to 2048 pixels wide, with the monitor one key away.
