# 14. An IR port

The two analog inputs that fed the USB microphone and the radio left the
board. In their place came an IR port: a demodulating receiver, the kind
found in every TV, and an IR LED. The receiver's output goes low while it
sees a 38 kHz carrier, so the ESP32 only has to time the pulses. The LED
needs the carrier itself.

## A receiver from MicroPython

We started from Peter Hinch's `ir_rx` for MicroPython, which was known to
work. Its idea is simple. An interrupt on both edges stores the time of
each edge. The first edge starts a timer, and when the timer ends, the
stored edges are decoded. The block has to outlast the longest frame (NEC,
67.5 ms) and end before the next one starts (an NEC repeat code comes
every 108 ms), so it lasts 100 ms.

In C the interrupt reads `esp_timer_get_time()` itself, inside the real
hardware interrupt. MicroPython's pin handlers usually run later, in the
interpreter, so our edge times are more accurate than the original's. The
timer became a task: the first edge wakes it, it sleeps 100 ms, takes the
edges and decodes them.

`ir_rx` handles one protocol per receiver. Its `IR_GET` helper only
guesses the protocol from the leader pulse. We combined the two: the
leader picks the protocol, and then that protocol's decoder from the
library runs.

- **NEC**, and **Samsung** with its shorter 4.5 ms leader: 32 bits in the
  length of the spaces. NEC repeat codes come back as `repeat` events.
- **Sony** SIRC, 12, 15 or 20 bits in the length of the marks.
- **Philips RC-5**: Manchester code, rebuilt bit by bit from the gaps.

Anything else is an `unknown` event, and its raw pulse timings are kept
for replay.

No remote was at hand while writing it, so the decoders were tested on the
Mac first. The test compiled `ir_rx.c` against stub ESP headers and fed it
made-up timings with ±8% jitter. Two of the cases: a Sony frame followed
by its repeat, and a 20-bit Sony frame of all ones, the one closest to
running into the next frame.

## And a transmitter

The LED is driven by the RMT peripheral. It generates the carrier in
hardware: 38 kHz for NEC, 40 kHz for Sony, 36 kHz for RC-5. A decoded
command is encoded again with each protocol's standard timings. Sony is
sent three times, 45 ms apart, as real remotes do. An unknown command is
replayed from its raw pulses. A second Mac test sent every protocol
through the encoder and back through the decoder, and all of them came out
unchanged.

## A program inside the firmware

The test program, `REMOTE`, shows the last command received and sends it
again on А↑. While it is being written, copying an ELF to the card on every
change is slow. So the boot menu learned about built-in programs: they are
listed first, in cyan. `exec_launch_main()` runs them exactly like a
program from `/BIN`, with the same keyboard, exit code and В↑ back to the
menu. `REMOTE` calls only what an SD-card program can, so it can move to
`apps/` as it is.

## Nothing arrives

The first test with an LG remote showed nothing, not even an `unknown`
burst. The code looked right, and an LG remote speaks plain NEC. So we
measured, one step at a time:

- every block logged on one line, noise included;
- a test command sent every second, for a loopback through our own LED;
- GPIO3 read as an ADC every 10 ms, to see the receiver's output voltage.

In the end the receiver was moved from GPIO3 to GPIO5, and it worked at
once. What upset GPIO3 we never found out. GPIO3 is a strapping pin, but
it only matters for JTAG on a board with a burned eFuse. All the debug code
was removed afterwards.

**Result:** an `ir` HAL that receives on GPIO5 and sends on GPIO4, a boot
menu that can hold programs built into the firmware, and a remote control
program to test them with.
