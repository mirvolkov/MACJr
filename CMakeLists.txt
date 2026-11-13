# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Each board backend under components/hal/ is a separate IDF component
# (named after its directory). Hardware HALs live in components/hal/,
# shared utilities (png, db, matrix, wav) in components/utils/.
# ESP-IDF only auto-discovers the immediate children of components/, so the
# nested ones need to be listed explicitly.
list(APPEND EXTRA_COMPONENT_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/components/hal/display"
  "${CMAKE_CURRENT_LIST_DIR}/components/hal/sdcard"
  "${CMAKE_CURRENT_LIST_DIR}/components/hal/speaker"
  "${CMAKE_CURRENT_LIST_DIR}/components/hal/mic"
  "${CMAKE_CURRENT_LIST_DIR}/components/hal/keyboard"
  "${CMAKE_CURRENT_LIST_DIR}/components/hal/i2c_bus"
  "${CMAKE_CURRENT_LIST_DIR}/components/hal/sdr"
  "${CMAKE_CURRENT_LIST_DIR}/components/hal/usbdev"
  "${CMAKE_CURRENT_LIST_DIR}/components/hal/ir"
  "${CMAKE_CURRENT_LIST_DIR}/components/hal/net"
  "${CMAKE_CURRENT_LIST_DIR}/components/utils/png"
  "${CMAKE_CURRENT_LIST_DIR}/components/utils/db"
  "${CMAKE_CURRENT_LIST_DIR}/components/utils/matrix"
  "${CMAKE_CURRENT_LIST_DIR}/components/utils/wav")

# Firmware version, shown by the boot menu (esp_app_get_description()).
set(PROJECT_VER "0.0.2")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(MACJr)
