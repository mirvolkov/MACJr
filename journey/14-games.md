# 14. Three games, and the accelerator that wasn't

MACJr grew three programs whose only job is play: `LIFE.ELF`, a full-screen
cellular automaton; `TETRIS.ELF`, a falling-block game drawn entirely in the
console; and `PLASMA.ELF`, the screen animation the boot menu runs when it is
idle. Two of them are strict console apps that touch nothing but the tty; the
third paints the panel directly. Between them they answered a hardware
question, stress-tested the 40x15 text screen, and gave the idle machine
something to do.

## Life is a convolution

Conway's Game of Life arrived first, and the interesting part was not the game
but the question it raised: the ESP32-S3 is sold on its vector "AI"
instructions, so what is the right way to make a cellular automaton fast?

Each cell's next state is the sum of its eight neighbours -- a 3x3 all-ones
convolution -- followed by a threshold. Counting the centre cell in, the 3x3
sum `S` runs 0..9 and the whole rule collapses to:

```
next = (S == 3) || (S == 4 && self)
```

That framing makes the chip's advertised hardware look perfect: a 3x3 conv is
exactly what `ESP-DL` / `ESP-NN` accelerate with the vector unit. So we looked
at three options.

- **A matrix multiply** is the wrong shape: it would need an NxN adjacency
  operator, O(N^2) for an O(N) stencil.
- **An `ESP-DL` depthwise conv2d** with an all-ones kernel genuinely works and
  genuinely uses the vector MACs -- but a one-bit automaton as int8 is eight
  times the memory, carries quantize/requantize machinery it doesn't need, and
  still wants a custom threshold pass. It's a lovely "Life as a CNN layer"
  demo, not a fast engine.
- **Bit-parallel SWAR** -- "SIMD within a register" -- wins outright.

## The full-adder trick

Pack the grid one bit per cell, 32 cells per `uint32_t`. You can't add packed
bits with `+`, because a carry would leak from one cell into its neighbour's
bit. So the neighbour count is built from XOR/AND full-adders that keep every
cell's arithmetic inside its own column:

```c
/* self + left + right of one row -> 2-bit count (s1,s0) */
s0 = l ^ c ^ r;
s1 = (l & c) | (l & r) | (c & r);
```

Three such row-counts are summed into the 4-bit `S`, and the rule becomes pure
boolean logic. Horizontal neighbours are `x<<1` / `x>>1` with the boundary bit
carried across words; the grid wraps, so edges are toroidal. The result
advances **32 cells per word** with no branches and no lookup. The whole
76,800-cell board is a fraction of a millisecond per generation -- the panel
blit, not the compute, is the limit. The grid is packed into the console HAL's
1-bpp mask and drawn in one call, the panel mapping bit=1 to green.

So the accelerator the chip is marketed on is the *wrong* tool here; the plain
ALU, used as a 32-wide vector of one-bit lanes, beats it on both speed and
memory. Vector-unit and matrix-multiply versions were written and timed against
it anyway; the plain register won, so it is the one that stayed.

Two old traps surfaced on the way. The first build spent **two to three seconds
on a black screen** at launch, burning CPU: the seed called `esp_random()` once
per cell, 76,800 times, and with the radio off the hardware RNG is throttled for
entropy. Pulling one hardware random and fanning it out with a cheap xorshift
dropped startup under a millisecond. Then loading any *other* program began
failing with `ESP_ERR_NO_MEM` -- Life's ~29 KB of grid buffers, resident in
internal RAM while it was a built-in, had dropped the free SRAM below the
contiguous block the ELF loader needs for a stack. Moving the buffers to PSRAM,
allocated only while Life runs, gave it back. Life then graduated to `LIFE.ELF`
on the SD card, its sparseness riding in as `argv[1]`.

## Tetris, in 40x15 characters

Life draws pixels; Tetris had to live inside the text grid. The screen is
**40 columns by 15 rows** of 8x16 glyphs, and a console app may touch only the
tty -- no panel access -- so the whole game is CP866 box-drawing and block
glyphs with ANSI colour, rebuilt into one buffer and flushed each frame.

The classic 10x20 well does not fit 15 rows, so the board is **10 wide by 13
tall**, two characters per cell (`\xDB\xDB`, two full blocks) so the cells read
as square, bordered with the double-line box glyphs `\xC9\xCD\xBB\xBA\xC8\xBC`.
That leaves a column to the right for the status panel: the next piece, score,
lines and level. The seven tetrominoes are stored as four cells plus a bounding
box, and rotation is computed rather than tabled -- `(x,y) -> (box-1-y, x)` per
quarter turn -- with the usual wall-kick offsets so a piece against a wall
still turns.

The controls map onto the MK-52's digit keys, read non-blocking from
`/dev/kbrd`: **1** and **3** move, **2** soft-drops, **5** rotates, **0**
pauses, В↑ hard-drops and Cx exits. Like every console program it yields the
screen while the system monitor (F+K) is open, and picks up where it left off
when the monitor closes.

## Plasma: something to do while idle

`PLASMA.ELF` is the oldest of the three and the simplest: a screen animation
that runs until any key, in two modes. The default is a **matrix rain** --
digits, Latin and CP866 Cyrillic capitals falling down the 40 columns, each
drop with a bright-white head, a green neck and a fading green trail, a few
trail glyphs flickering each frame. The `-r` mode is a **rainbow**: random
glyphs in random bright colours, a fresh field every frame. Both are built from
the same xorshift PRNG (the loader exports no `rand()`), written as cursor-move
plus colour plus glyph into one buffer per frame.

It is also the system's screensaver: the boot menu launches `PLASMA.ELF` with
no arguments when it sits idle, so a resting MACJr shows the rain until a key
brings the navigator back.

**Result:** a 320x240 Game of Life in a 3.3 KB program and a concrete answer to
"which accelerator?" -- for a one-bit automaton, the register itself; a Tetris
that fits a 10x13 well into a 40x15 text console with nothing but glyphs; and a
falling-rain screensaver for the idle machine.
