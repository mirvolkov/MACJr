# The MACJr journey

MACJr is an educational Unix-like OS for the ESP32-S3, built step by step,
and it is going to live inside the case of a Soviet *Elektronika MK-52*
programmable calculator. These short stories are the log of how the board
support came together: what we tried, what broke, and how we fixed it.

| # | Story | In one line |
|---|-------|-------------|
| 1 | [The keyboard matrix](01-keyboard-matrix.md) | An MCP23018 scanner, two dead rows, and a `/dev/kbrd` device |
| 2 | [A microphone over USB](02-mic-and-uac.md) | ADC with DMA streamed to a PC as a USB audio device |
| 3 | [A tiny radio](03-sdr-si5351.md) | Si5351 driver from the datasheet: 7.055 MHz in quadrature |
| 4 | [The boot menu](04-boot-menu.md) | A start screen that exposed two display bugs; later, double buffering |
| 5 | [Faster boot](05-faster-boot.md) | Config tuning, and a PSRAM test we rolled back |
| 6 | [The system monitor](06-system-monitor.md) | A live overlay: CPU, RAM, temperature, SD card |
| 7 | [Mapping the MK-52 keys](07-mk52-key-mapping.md) | 32 presses that turned key codes into calculator keys |
| 8 | [An MK-52 inside the MK-52](08-mk52-calculator.md) | The calculator itself, tested on the Mac first |
| 9 | [Catching a crash that hid from us](09-the-psram-hang.md) | A heisenbug found with JTAG, and its cause: the LCD reset on a PSRAM pin |
| 10 | [The SD card as a USB drive](10-usb-sd-drive.md) | One TinyUSB stack, two devices: microphone or card reader |
| 11 | [A picture from the SD card](11-png-port-layout.md) | Our own streaming PNG decoder on the ROM's inflater |
| 12 | [A config file of our own](12-config-db.md) | A key=value store on the SD card, with a crash-safe rewrite |
| 13 | [Programs from the SD card](13-programs-from-sd.md) | ELF apps loaded into PSRAM and run on the second core |
| 14 | [An IR port](14-ir-remote.md) | A receiver ported from MicroPython, an RMT transmitter, and a remote control |
| 15 | [Waiting without polling](15-flashdb-select-watch.md) | FlashDB for config, `select()` on the keyboard, database watches, matrix rain |

For the compact list of every step so far, open issues and what comes
next, see [roadmap.txt](roadmap.txt).

The hardware: ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB PSRAM), a 320x240
ST7789 display (a 40x15 text screen with an 8x16 font), an SD card, an
MCP23018 I/O expander wired to the MK-52 keyboard, an Si5351 clock
generator driving a Tayloe detector, and an IR port (receiver on GPIO5,
LED on GPIO4) that replaced the two analog inputs.
