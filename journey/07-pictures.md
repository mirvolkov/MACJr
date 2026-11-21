# 7. Pictures: in, out, and a viewer

The board needed to show its own port layout, so the first picture program
displayed one hardcoded file, `IMG/layout.png`, scaled to fit. The same `png`
component later learned to write, which gave us screenshots, and later still
the one-file program grew into a general viewer -- `IMAGEVIEWER.ELF`, which
takes any picture and shows it, PNG or JPEG.

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
this bus. Rendering the text cells would miss any picture on screen. So the
display HAL keeps a shadow: 150 KB in PSRAM, updated with every span it sends.
Whatever reaches the glass is in that copy, in the panel's byte-swapped RGB565.

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

At first the only sign of a screenshot was the "Screenshot taken" window, and
it came only after the file was written. Encoding 230 KB and writing it to the
card takes a noticeable moment, so the chord felt laggy. Now the save is split
in two. `screenshot_grab()` copies the shadow framebuffer, which is fast. Only
then does a "Taking screenshot" window go up, so it never ends up in the
picture. `screenshot_write()` encodes the file and saves it, and the same
window changes to the file name, or to the reason it failed.

## From one picture to any picture

The port-layout program knew one path and nothing else. But its drawing code --
decode row by row, scale to fit, blit behind the tty -- was already general; all
it lacked was an argument. So it became `IMAGEVIEWER.ELF`, taking the image
path as `argv[1]`, and the navigator learned to hand it one: pressing В↑ on an
image file returns that file's path, and the firmware routes it to the viewer,
exactly the way a `.py` goes to Python and a sound goes to the WAV player. The
hardcoded port app is gone; its `layout.png` is simply an image on the card,
opened like any other. A picture is still one of the few things allowed to draw
on the display HAL instead of the console.

## JPEG for free, from the ROM

The viewer only spoke PNG. Adding JPEG looked like vendoring a decoder, but the
ESP32-S3 mask ROM already contains one: **TJpgDec**, with `jd_prepare` and
`jd_decomp` at fixed addresses the linker script provides -- the same free
lunch as the ROM inflater behind PNG. No new dependency, no code to carry.

The only work was shape-matching. TJpgDec hands pixels back in **MCU
rectangles** -- 8x8 or 16x16 blocks, left to right then top to bottom -- while
the viewer wants whole scanlines. So the wrapper gathers one MCU-row-tall band
across the full width, and when the next band begins it emits the finished
band's rows through the same `on_header` / `on_row` callbacks the PNG path uses.
It also leans on a feature the ROM decoder already has: a power-of-two
**down-scale during decode**, so an oversized photo is shrunk toward the
320x240 screen before the pixels are ever assembled. The viewer picks the
decoder by file extension; everything else is shared.

## Half a second to show a thumbnail

The first thing anyone noticed was speed: an image appeared in rough horizontal
chunks over about half a second. The chunks were a red herring -- they are just
the display HAL's six 40-row DMA bands flushing as the decode streams, maybe
20 ms of SPI in total. The half second was spent getting the file off the card.

The SD card is wired as **SPI, one bit wide, at 20 MHz**, and the decoder read
it a kilobyte at a time. A 70 KB picture is then about seventy separate SD
transactions, each with its own command and response latency -- and that, not
the inflate and not the panel, was most of the wall-clock time. The fix was one
line: give the `FILE *` a 16 KB standard-I/O buffer, so each refill is a single
large multi-sector read instead of dozens of small ones. The same buffer went
on the JPEG path.

Raising the SPI clock to 40 MHz looked like the obvious second win, and it is
one line more -- but on the hand-wired board the card then **failed to mount**
at all. Forty megahertz over perfboard jumpers is past what the signal can
take, so that line came straight back out. The clock stays at 20 MHz; the big
read is what matters.

**Result:** any PNG or JPEG up to 2048 pixels wide on screen, started from the
navigator on any picture; screenshots of anything on the panel, which soon paid
off by [exposing bad box corners](03-display-and-tty.md); and a load that went
from half a second to a fraction of it, from reading the card in large gulps
rather than sips.
