This is research project for esp32-s3 board
Final goal is to make educational unix-like OS with terminal output mode, set of libs and crosscompiles
Work goes step by step, initial step is full hardware support
Each iteration should be documented (by user request) with compact recap what and how was done in .txt file in roadmap folder

With 320x240 screen size and 8x16 glyph size available text mode is 40x15 symbols. Keep this in mind and always fit under this size

There is also a macOS host build (SDL2) for running the same tty/vc/font stack without hardware, in macos/. It's a separate plain-CMake project (not wired into the ESP-IDF root CMakeLists.txt):
  brew install sdl2
  cmake -S macos -B build-host && cmake --build build-host
  ./build-host/macjr_host
ESP-IDF-specific code is isolated behind `#ifdef ESP_PLATFORM` (set automatically by the IDF build) so the portable tty/vc/font/demo-screen sources are shared unmodified between the two targets; platform-only code (FreeRTOS primitives, display HAL, flash/heap stats) lives in its own file per platform.
