# MACJr

MACJr is a vibecoded research project: an educational Unix-like OS for the
ESP32-S3, built step by step. It is going to live inside the case of a
Soviet *Elektronika MK-52* programmable calculator, using the calculator's
own keyboard.

The system is a text console of 40x15 characters on a 320x240 display.
A small firmware holds the hardware drivers, the tty, a boot menu, a system
monitor and a program loader. Everything else is a program: an ELF file on
the SD card, started from the menu. The final goal is a proper kernel, a set
of libraries and a cross-compiler.

## The motherboard

<img src="images/motherboard_v1.jpeg" alt="MACJr motherboard v1: an ESP32-S3 board, display, SD card slot and amplifier hand-wired on a perfboard" width="480">

This is the first version, hand-wired on a 10x15 cm perfboard and showing a
test pattern on the display.

| Part | What it does |
|---|---|
| ESP32-S3-WROOM-1 N16R8 | The computer: two cores at 240 MHz, 16 MB flash, 8 MB octal PSRAM |
| ST7789 display, 320x240 | The console, with an 8x16 font and CP866 glyphs |
| SD card | Programs, pictures, screenshots and settings |
| MAX98357A I2S amplifier | Sound |
| MCP23018 I/O expander | Reads the 32 keys of the MK-52 over I2C |
| Si5351 clock generator | Local oscillator for a Tayloe detector radio |
| IR receiver and LED | Receives and sends remote control commands |

The last three parts are not in this photo.

## The journey

It started with the keyboard. The MK-52's 32 keys went to an MCP23018
expander, so two wires could read them all. Two rows stayed silent, and
swapping them in software changed nothing. A small wiring probe showed the
fault was in the wires. The keyboard then became a Unix-style device,
`/dev/kbrd`, that any program can read like a file.

Next came the radio side: an Si5351 driver written from the datasheet, tuned
to 7.055 MHz in quadrature, and a two-channel ADC streamed to a computer as a
USB microphone. A boot menu let us choose between these modes, and its first
screen exposed two display bugs at once: a doubled title and green text
that came out magenta. A system monitor followed, an overlay with CPU, memory
and temperature that puts the screen back when it closes.

Then the board became the calculator whose case it will live in. We pressed
all 32 keys in order to name them, and wrote an MK-52 from its manuals, tested
on the Mac first. To match the real machine exactly it had to truncate
results instead of rounding, with π cut off at `3.1415926`.

Soon after, the board started resetting itself with no backtrace. Adding log
lines made the crash vanish, so we attached a JTAG debugger and found CPU0
stuck inside the PSRAM allocator with its heap corrupted. The cause only
surfaced later, when the crash came back: the LCD reset wire was on GPIO35,
one of the octal PSRAM's data lines. Moving it to GPIO14 ended the crashes.

The board also learned to share. One USB stack now serves as a microphone
or as a card reader for the SD card. A streaming PNG decoder, built on the
inflater in the chip's ROM, shows pictures in about 45 KB of RAM. Settings
went from SQLite (too heavy) to our own text file and finally to FlashDB.

The biggest step was programs as files, the way DOS had them. With
Espressif's ELF loader, apps load from the SD card into PSRAM and run on the
second core, and the firmware shrank to the essentials. New apps no longer
need a reflash, and a build-time checker catches what the loader would
reject.

The system then grew more Unix-like. An IR port replaced the analog inputs.
`select()` came to the keyboard, along with database watches, so programs can
wait instead of polling. Wi-Fi arrived with a scrolling list window, the
console's first real widget. Key chords gave the system a kill: F and Cx end
any program, and its descriptors, memory, USB and SD card are put back.
Last came PNG screenshots from a shadow framebuffer, and the first zoomed-in
picture showed double-line box corners that nobody had noticed were wrong.

So far this is step one, full hardware support. The radio is unfinished,
because the USB microphone never worked well. Ahead lie the kernel, with
processes, memory and file systems, then a standard library, a cross-compiler
and more programs.

The full log is in 11 short topic stories, with code and real logs, in
[journey/](journey/README.md).
