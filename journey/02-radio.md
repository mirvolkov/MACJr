# 2. A tiny radio

The receiver is a Tayloe detector: a fast switch that mixes the antenna
signal down to two audio-frequency signals, I and Q. It needs a local
oscillator in two copies 90 degrees apart, and its output has to reach a
computer. So this step had two halves: an Si5351 clock, and a USB microphone.

## The Si5351, from the datasheet

The clock chip sits on the same I2C bus as the keyboard, so the bus moved
into its own component. `i2c_bus_get()` creates it on first use. Our first
version guarded that with a lock written as a temporary value, which protects
nothing because each call got a fresh lock. The fix was a single static lock.

The driver follows Skyworks note AN619 directly. The chip multiplies its
25 MHz crystal up to 600-900 MHz (a PLL), then divides it down for each
output. For a clean 90-degree shift the divider must be an even whole number
of at most 126. For 7.055 MHz:

```
N   = 126                         even, at most 126
PLL = 7.055 MHz x 126 = 888.93 MHz   inside 600-900 MHz
PLL / 25 MHz = 35.557...          written as a + b/c
```

The quadrature trick is the phase-offset register. Setting CLK1's offset to
N delays it by a quarter of a period, and it only takes effect after a PLL
reset:

```c
write_reg(166, n);        /* CLK1 phase offset = N -> 90 degrees */
write_reg(177, 0x20);     /* reset PLL A: apply the phase */
write_reg(3, ~0x03);      /* enable CLK0 and CLK1 */
```

## A two-channel microphone

The easiest way to look at I and Q is to send them to a PC as a USB
microphone and open them in any SDR program. The ADC runs in continuous mode
and DMA drops the samples into memory. `mic_read()` pairs them into stereo
frames, scales them to 16 bits and removes the DC bias. Both inputs had to be
on ADC1, because continuous mode does not work on ADC2 on the S3 (a chip
erratum).

As soon as USB audio started, the board vanished from the PC and flashing
stopped working. The S3 has one internal USB interface, normally the
serial/JTAG port, and the audio driver takes it over. So USB audio starts
only when chosen, and the board's second USB-C port, a separate USB-serial
bridge, still works for flashing.

## Artefacts on the waterfall

The first recordings showed vertical lines. The ADC cannot run at exactly
16 kHz, because its clock divider is rounded:

```
80 MHz / 16 / 2 / 78 = 32051 conversions/s  ->  16025.6 Hz per channel
```

That is 0.16 % fast, and when the driver's buffer overflows it throws the
whole buffer away. Regular gaps look exactly like those lines. A ring buffer
with a self-adjusting resampler made the chip reboot, so we rolled it back
and took a simpler step: 1 ms DMA frames and 2 ms USB packets, with a
1000 Hz FreeRTOS tick. That only shortens the gaps.

**Result:** an `sdr` component with `sdr_set_freq(hz)` covering 4.762 to
150 MHz, and stereo audio on any PC. Still open: the I/Q outputs were never
checked on a scope or with a real antenna, the rate mismatch needs a proper
fix, and the Si5351 keeps running after RADIO ends, since its driver has no
stop call. The two analog inputs have since left the board for the
[IR port](10-ir-remote.md).
