# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# components/esp32s3_hal/display is a separate IDF component (named
# "display") nested under components/esp32s3_hal/ (a grouping directory for
# ESP32-S3-specific backends; the macjr_hal_display_t interface itself lives
# in the portable tty component). ESP-IDF only auto-discovers the immediate
# children of components/, so it needs to be listed explicitly.
list(APPEND EXTRA_COMPONENT_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/components/esp32s3_hal/display"
  "${CMAKE_CURRENT_LIST_DIR}/components/esp32s3_hal/sdcard")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(MACJr)
