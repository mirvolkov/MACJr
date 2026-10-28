This is research project for esp32-s3 board
Final goal is to make educational unix-like OS with terminal output mode, set of libs and crosscompiles
Work goes step by step, initial step is full hardware support
Each iteration should be documented (by user request): add the step to journey/roadmap.txt (single compact roadmap file) and, when asked, a brief narrative story as journey/NN-<topic>.md

With 320x240 screen size and 8x16 glyph size available text mode is 40x15 symbols. Keep this in mind and always fit under this size

Target is ESP32-S3 only (ESP-IDF). Board backends live under components/hal/ (display, sdcard, speaker, mic, matrix, keyboard), each a separate IDF component listed in EXTRA_COMPONENT_DIRS of the root CMakeLists.txt. Entry point is main/main.c.
