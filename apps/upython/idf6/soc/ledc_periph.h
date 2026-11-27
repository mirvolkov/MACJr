/*
 * ESP-IDF 6 moved soc/ledc_periph.h to hal/ledc_periph.h (esp_hal_ledc);
 * MicroPython's ports/esp32/machine_pwm.c, written for IDF 5, includes the
 * old path. Same table (ledc_periph_signal), new place.
 */
#pragma once
#include "hal/ledc_periph.h"
