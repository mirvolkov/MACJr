# 14. Life, and the accelerator that wasn't

Conway's Game of Life came to MACJr as `LIFE.ELF`, a full-screen 320x240
cellular automaton. The interesting part was not the game but the question it
raised: the ESP32-S3 is sold on its vector "AI" instructions, so what is the
right way to make a cellular automaton fast?

## Life is a convolution

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
memory.

## Two old traps, again

The first build spent **two to three seconds on a black screen** at launch,
burning CPU. The culprit was the seed: `esp_random()` called once per cell,
76,800 times. With the radio off the hardware RNG is throttled for entropy, so
each read costs tens of microseconds. The fix was to pull one hardware random
and fan it out with a cheap xorshift PRNG -- startup dropped to under a
millisecond. The seed density became a parameter too (`argv[1]`, a 2^-n live
fraction, default 1/8), tunable live with the x and divide keys.

Then loading any *other* program started failing with `ESP_ERR_NO_MEM`. Life
was first built as a firmware built-in, and its ~29 KB of grid buffers sat in
internal RAM permanently -- just enough to drop the free internal SRAM below
the contiguous block the ELF loader needs for a program's stack. The same
squeeze that once bit MicroPython. Moving the buffers to PSRAM, allocated only
while Life runs, gave it back.

Finally Life graduated from a built-in to `LIFE.ELF` on the SD card: the
engine (`life_swar.c`) and the app (`life.c`) build into one ELF, the firmware
loses another resident program, and the sparseness argument rides in as
`argv[1]`.

**Result:** a 320x240 Game of Life in a 3.3 KB program, bit-parallel across
32-cell words, and a concrete answer to "which accelerator?" -- for a one-bit
automaton, the register itself.
