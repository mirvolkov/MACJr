This is research project for esp32-s3 board
Final goal is to make educational unix-like OS with terminal output mode, set of libs and crosscompiles
Work goes step by step, initial step is full hardware support
Each iteration should be documented (by user request): add the step to journey/roadmap.txt (single compact roadmap file) and, when asked, a brief narrative story as journey/NN-<topic>.md

With 320x240 screen size and 8x16 glyph size available text mode is 40x15 symbols. Keep this in mind and always fit under this size

Apps are console apps: they draw only through the tty (text, ANSI escapes, CP866 glyphs incl. box drawing), never on the display HAL directly. Graphics on the HAL only in specially agreed cases (so far: the image viewer, apps/imageviewer, launched from the navigator on an image file).

Target is ESP32-S3 only (ESP-IDF). Board backends live under components/hal/ (display, sdcard, speaker, mic, matrix, keyboard), each a separate IDF component listed in EXTRA_COMPONENT_DIRS of the root CMakeLists.txt. Entry point is main/main.c.

Apps are programs, not firmware: apps/<name>/*.c is built by apps/Makefile (`make`, after `idf.py build`) into <NAME>.ELF, which the firmware loads from /bin with espressif/elf_loader (components/exec) and lists in the boot menu. /bin is the board's flash partition for programs ("apps" in partitions.csv), written over Wi-Fi (`make deploy HOST=<ip>`, which also updates the firmware) or the USB cable (`make flash-apps PORT=<port>`); the SD card holds files and data (`make install CARD=/Volumes/X` copies the cc headers). A program's entry is main(argc, argv, tty) (sdk/include/macjr.h); firmware functions it calls must be listed in components/exec/exports.c (the Makefile checks). The firmware keeps only the HALs, tty, boot menu, system monitor and loader.
