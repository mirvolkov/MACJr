# 16. A picture viewer, and a slow read

Pictures had been on the board since story 7 -- a streaming PNG decoder built
on the ROM's inflater -- but only as one hardcoded program, `PORT.ELF`, that
showed a single file: the board's port layout. That program knew one path and
nothing else. This step turned it into a real viewer: an app that takes a file
and shows it, launched from the navigator like everything else.

## From one picture to any picture

The old app's drawing code was already general -- decode row by row, scale to
fit, blit behind the tty. All it lacked was an argument. So it became
`PNGVIEWER.ELF`, taking the image path as `argv[1]`, and the navigator learned
to hand it one: pressing В↑ on an image file now returns that file's path, and
the firmware routes it to the viewer, exactly the way a `.py` goes to Python
and a sound goes to the WAV player. The hardcoded port app is gone; its
`layout.png` is simply an image on the card, opened the same way as any other.
A picture is still one of the few things allowed to draw on the display HAL
instead of the console.

## JPEG for free, from the ROM

The viewer only spoke PNG. Adding JPEG looked like it meant vendoring a
decoder, but the ESP32-S3 mask ROM already contains one: **TJpgDec**, with
`jd_prepare` and `jd_decomp` at fixed addresses the linker script provides --
the same kind of free lunch as the ROM inflater behind PNG. No new dependency,
no code to carry.

The only work was shape-matching. TJpgDec hands pixels back in **MCU
rectangles** -- 8x8 or 16x16 blocks, left to right then top to bottom -- while
the viewer wants whole scanlines, top to bottom. So the wrapper gathers one
MCU-row-tall band across the full width, and when the next band begins it emits
the finished band's rows through the same `on_header` / `on_row` callbacks the
PNG path uses. It also leans on a feature the ROM decoder already has: a
power-of-two **down-scale during decode**, so an oversized photo is shrunk
toward the 320x240 screen before the pixels are ever assembled. The viewer then
picks the decoder by file extension; everything else is shared.

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

**Result:** a general image viewer (PNG and JPEG) in about 4 KB, started from
the navigator on any picture, with the port-layout app retired to an ordinary
file -- and a picture that now loads in a fraction of the time, from reading the
card in large gulps rather than sips.
