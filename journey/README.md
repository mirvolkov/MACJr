# The MACJr journey

MACJr is an educational Unix-like OS for the ESP32-S3, built step by step,
and it is going to live inside the case of a Soviet *Elektronika MK-52*
programmable calculator. These short stories, one per topic, are the log
of how the board support came together: what we tried, what broke, and how
we fixed it.

| # | Story | In one line |
|---|-------|-------------|
| 1 | [The keyboard](01-keyboard.md) | An MCP23018 scanner, two dead rows, `/dev/kbrd`, key names and chords |
| 2 | [A tiny radio](02-radio.md) | Si5351 from the datasheet, and I/Q streamed to a PC as a USB microphone |
| 3 | [The display and the tty](03-display-and-tty.md) | Boot menu, double buffering, the monitor, list window, glyphs and box corners |
| 4 | [Faster boot and the PSRAM hang](04-boot-and-psram.md) | Config tuning, then a heisenbug found with JTAG: the LCD reset on a PSRAM pin |
| 5 | [An MK-52 inside the MK-52](05-mk52-calculator.md) | The calculator itself, tested on the Mac first |
| 6 | [USB: one stack, two personalities](06-usb.md) | Microphone or card reader, and USB put back when the program ends |
| 7 | [Pictures: PNG in and out](07-pictures.md) | A streaming decoder on the ROM's inflater, a shadow framebuffer, screenshots |
| 8 | [The config database](08-config-db.md) | SQLite, a text file, then FlashDB, with watches and a safe unmount |
| 9 | [Programs from the SD card](09-programs.md) | ELF apps in PSRAM on the second core, and how to kill them cleanly |
| 10 | [An IR port](10-ir-remote.md) | A receiver ported from MicroPython, an RMT transmitter, and a remote control |
| 11 | [On the air](11-wifi.md) | The Wi-Fi station and a program to join a network |

For the compact list of every step so far, open issues and what comes
next, see [roadmap.txt](roadmap.txt).

The hardware: ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB PSRAM), a 320x240
ST7789 display (a 40x15 text screen with an 8x16 font), an SD card, an
MCP23018 I/O expander wired to the MK-52 keyboard, an Si5351 clock
generator driving a Tayloe detector, and an IR port (receiver on GPIO5,
LED on GPIO4) that replaced the two analog inputs.
