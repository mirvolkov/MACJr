# Header-only: the abstract macjr_hal_display_t interface that every
# concrete display backend (e.g. esp32_st7789) implements and that tty/
# renders through. Backends live in their own components alongside this one
# (see components/esp32s3hal/esp32_st7789) and REQUIRE "esp32s3hal" for the
# interface. Named to avoid colliding with ESP-IDF's own "hal" component.
idf_component_register(
  INCLUDE_DIRS "include"
)
