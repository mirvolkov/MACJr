/*
 * The port's part of the machine module (MICROPY_PY_MACHINE_INCLUDEFILE,
 * included by extmod/modmachine.c), as ports/esp32/modmachine.c is on the
 * esp32 port, reduced to what this board has so far: Pin, and idle().
 * PWM, ADC, SoftI2C, SoftSPI, Signal and time_pulse_us come from
 * extmod/modmachine.c itself.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ports/esp32/machine_pin.h"
#include "ports/esp32/modmachine.h"

#define MICROPY_PY_MACHINE_EXTRA_GLOBALS \
    { MP_ROM_QSTR(MP_QSTR_Pin), MP_ROM_PTR(&machine_pin_type) },

/* machine.idle(): give the CPU away for a tick */
static void mp_machine_idle(void) {
    vTaskDelay(1);
}
