# Standalone, plain-CMake build for the macOS/SDL2 host target -- runs the
# same tty/vc/font stack and demo screen as the ESP32 firmware in an SDL2
# window, for development without hardware. Deliberately NOT wired into the
# top-level (ESP-IDF) CMakeLists.txt: that one calls into
# $ENV{IDF_PATH}/tools/cmake/project.cmake and assumes the IDF toolchain, so
# this target gets its own separate build:
#
#   cmake -S macos -B build-host
#   cmake --build build-host
#   ./build-host/macjr_host
#
cmake_minimum_required(VERSION 3.16)
project(macjr_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(macjr_host
  ${ROOT}/components/tty/src/vc.c
  ${ROOT}/components/tty/src/ldisc_ntty.c
  ${ROOT}/components/tty/src/tty.c
  ${ROOT}/components/tty/src/hal_blit.c
  ${ROOT}/components/tty/src/font_builtin.c
  ${ROOT}/components/macos_hal/display/display_hal.c
  ${ROOT}/main/ide_screen.c
  ${ROOT}/main/main_macos.c
)

target_include_directories(macjr_host PRIVATE
  ${ROOT}/components/tty/include
  ${ROOT}/components/macos_hal/display
  ${ROOT}/main
)

target_link_libraries(macjr_host PRIVATE SDL2::SDL2 Threads::Threads)
