# 16. The SDR moves to a branch

The Wi-Fi-band SDR -- `ESPSDR.ELF`, the 2.4 GHz spectrogram built on the chip's
receiver ADC dump engine -- came off master and now lives on the `esp-sdr`
branch. Not because it stopped working, but because of what it cost just to
keep it loadable.

## 64 KB, whether you use it or not

The dump engine writes captured IQ into a **fixed 64 KB SRAM bank** at
`0x3fcd0000`, and during each ~0.2 ms capture the CPU loses the *whole* bank --
reads anywhere in it return garbage, writes are lost. So nothing else may live
there while the SDR runs, and the firmware reserves it at boot,
unconditionally, whether or not you ever open the spectrogram:

```c
SOC_RESERVE_MEMORY_REGION(0x3fcd0000, 0x3fce0000, espsdr_dump);  // 64 KB, always
```

That dropped the free internal SRAM from ~335 KB to ~260 KB, visible in the
system monitor at idle with no app running.

## Why it can't be made on-demand

- The target is a **fixed hardware bank**, chosen by an owner-register bit, not
  a programmable address -- so it can't be pointed at a `malloc`'d buffer.
- You can't tell the allocator to avoid a fixed address only sometimes, and
  there's no "remove heap region" call to evacuate it when the SDR starts.
- The whole 64 KB is owned by the engine during a capture, so you can't even
  shrink the reservation and lend the rest to the heap -- a concurrent read of
  the lent part during a capture would come back corrupt.

The 64 KB is simply the price of having the SDR launchable at any moment.
(`CONFIG_ESP_PHY_ENABLE_CERT_TEST`, which the SDR needs, turned out to be nearly
free: the PHY poke functions it exposes -- `set_chanfreq`, `force_rx_gain`,
`rom_pbus_*` -- already live in `libphy` for Wi-Fi; the flag only keeps the
linker from stripping them.)

## The split

So the SDR app, the `espsdr` capture HAL, its four exported calls and the
`CERT_TEST` flag all moved to the **`esp-sdr`** branch, and master got its
64 KB back. The spectrogram is one `git switch` away when it's wanted; the
mainline stays lean.

**Result:** ~75 KB of internal SRAM returned to master; the Wi-Fi-band SDR
preserved whole on the `esp-sdr` branch.
