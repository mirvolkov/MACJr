# 10. An IR port

The two analog inputs that fed the microphone and the radio left the board. In
their place came an IR port: a demodulating receiver, the kind found in every
TV, and an IR LED. The receiver's output goes low while it sees a 38 kHz
carrier, so the ESP32 only has to time the pulses. The LED needs the carrier
itself.

## A receiver from MicroPython

We started from Peter Hinch's `ir_rx`. An interrupt on both edges stores the
time of each edge, and the first edge starts a 100 ms block: longer than the
longest NEC frame (67.5 ms), and shorter than the gap to an NEC repeat (108 ms).
Then the stored edges are decoded. In C the interrupt reads
`esp_timer_get_time()` inside the real hardware interrupt, so our edge times are
more accurate than the original's.

`ir_rx` handles one protocol per receiver. We combined it with its `IR_GET`
helper: the leader pulse picks the protocol, and that protocol's decoder runs.

- **NEC**, and **Samsung** with its shorter leader: 32 bits in the length of the
  spaces, with `repeat` events.
- **Sony** SIRC, 12, 15 or 20 bits in the length of the marks.
- **Philips RC-5**: Manchester code, rebuilt bit by bit from the gaps.

Anything else is an `unknown` event, with raw timings kept for replay. No remote
was at hand, so the decoders were tested on the Mac with made-up timings and
±8% jitter.

## And a transmitter

The RMT peripheral generates the carrier in hardware: 38 kHz for NEC, 40 kHz for
Sony, 36 kHz for RC-5. A decoded command is encoded again with its protocol's
timings, and an unknown one is replayed from its raw pulses. A second Mac test
sent every protocol through the encoder and back through the decoder, and all
came out unchanged.

## Nothing arrives

`REMOTE` shows the last command received and sends it again on А↑. The first
test with an LG remote showed nothing, not even an `unknown` burst. We measured
step by step: logging every block, a loopback through our own LED, and reading
GPIO3 as an ADC. In the end the receiver moved from GPIO3 to GPIO5, and it
worked at once. What upset GPIO3 we never found out.

**Result:** an `ir` HAL that receives on GPIO5 and sends on GPIO4, and a remote
control program to test it with.
