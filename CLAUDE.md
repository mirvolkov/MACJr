This is research project for esp32-s3 board
Final goal is to make educational unix-like OS with terminal output mode, set of libs and crosscompiles
Work goes step by step, initial step is full hardware support
Each iteration should be documented (by user request): add the step to journey/roadmap.txt (single compact roadmap file) and, when asked, a brief narrative story as journey/NN-<topic>.md

With 320x240 screen size and 8x16 glyph size available text mode is 40x15 symbols. Keep this in mind and always fit under this size

Apps are console apps: they draw only through the tty (text, ANSI escapes, CP866 glyphs incl. box drawing), never on the display HAL directly. Graphics on the HAL only in specially agreed cases (so far: the PNG viewer, apps/port).

Target is ESP32-S3 only (ESP-IDF). Board backends live under components/hal/ (display, sdcard, speaker, mic, matrix, keyboard), each a separate IDF component listed in EXTRA_COMPONENT_DIRS of the root CMakeLists.txt. Entry point is main/main.c.

Apps are programs on the SD card, not firmware: apps/<name>/*.c is built by apps/Makefile (`make`, after `idf.py build`; `make install CARD=/Volumes/X`) into <NAME>.ELF, which the firmware loads from /BIN with espressif/elf_loader (components/exec) and lists in the boot menu. A program's entry is main(argc, argv, tty) (sdk/include/macjr.h); firmware functions it calls must be listed in components/exec/exports.c (the Makefile checks). The firmware keeps only the HALs, tty, boot menu, system monitor and loader.
