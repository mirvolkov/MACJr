# 2. A microphone over USB

The radio part of the project produces two audio-frequency signals, I and
Q. The easiest way to look at them is to send them to a PC as a USB
microphone and open them in any SDR program. So we built a two-channel
"mic": the ADC samples both inputs, and the ESP32's USB port presents
itself as a standard USB audio (UAC 2.0) device.

## Sampling with DMA

The ADC runs in continuous mode: it converts left, right, left, right on
its own, and DMA drops the results into memory without the CPU touching
each sample. `mic_read()` pairs them into stereo frames, scales 12-bit
values to 16-bit and removes the DC bias with a small high-pass filter.

The inputs ended up on GPIO3 and GPIO4. We could not use the ADC2 pins:
on the ESP32-S3, continuous (DMA) mode does not work on ADC2 because of a
chip erratum, so both channels had to be on ADC1.

## Where did my flash port go?

As soon as USB audio started, flashing stopped working: the board vanished
from the PC. The ESP32-S3 has only one internal USB interface. It is
normally the serial/JTAG port used for flashing, and the audio driver takes
it over. The fix was to keep USB audio out of the default boot: it now
starts only when chosen in the boot menu. The board's second USB-C port,
a separate USB-serial bridge chip, also works for flashing.

## Artefacts on the waterfall

The first recordings showed vertical lines and a weak signal. Reading the
ESP-IDF source showed why. The ADC cannot run at exactly 16 kHz: its clock
divider is rounded, so it really runs at

```
80 MHz / 16 / 2 / 78 = 32051 conversions/s  ->  16025.6 Hz per channel
```

That is 0.16 % fast, and when the driver's buffer overflows it throws away
the whole buffer at once. Regular gaps in a signal look exactly like those
vertical lines.

Our first fix, a ring buffer with a self-adjusting resampler, made the
chip reboot, so we rolled it back. For now we took the simpler step: 1 ms
DMA frames instead of 4 ms, and USB packets every 2 ms instead of 10 ms.
That needed a faster FreeRTOS tick (1000 Hz), because a 2 ms wait would
otherwise round down to zero ticks, the same trap as with the keyboard.

**Result:** stereo I/Q audio on any PC, with shorter gaps. Still open:
GPIO4 shows no signal yet, and the rate mismatch needs a proper fix. We
also researched a way to get more dynamic range: two copies of each
channel at different gains, stitched together in software.
