/*
 * MACJr HAL for the MicroPython embed port.
 */
#pragma once

/* sys_millis() is the tick source. Declared here rather than taken from
 * macjr.h, which would bring all of the tty in. */
unsigned sys_millis(void);
void sys_delay(unsigned ms);
static inline mp_uint_t mp_hal_ticks_ms(void) { return (mp_uint_t)sys_millis(); }
static inline void      mp_hal_delay_ms(mp_uint_t ms) { sys_delay(ms); }

/* Microseconds for time.ticks_us(), from esp_timer (exported). A delay
 * under a millisecond is a busy wait (bit-banged I2C/SPI need it); a
 * longer one sleeps on sys_delay(), which ends at a kill. */
#include <stdint.h>
#include "esp_rom_sys.h"
int64_t esp_timer_get_time(void);
static inline mp_uint_t mp_hal_ticks_us(void) { return (mp_uint_t)esp_timer_get_time(); }
static inline mp_uint_t mp_hal_ticks_cpu(void) { return mp_hal_ticks_us(); }
static inline void      mp_hal_delay_us(mp_uint_t us) {
    if (us < 1000) {
        esp_rom_delay_us(us);
    } else {
        sys_delay((us + 999) / 1000);
    }
}
#define mp_hal_delay_us_fast(us) esp_rom_delay_us(us)

/* An ESP-IDF error as OSError, as in the esp32 port (mphalport.c). */
#include "esp_err.h"
#define check_esp_err(code) check_esp_err_(code)
void check_esp_err_(esp_err_t code);

/* C-level pin HAL, as in MicroPython's esp32 port (ports/esp32/mphalport.h):
 * what SoftI2C, SoftSPI, Signal and time_pulse_us use. */
#include "py/obj.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#define MP_HAL_PIN_FMT "%u"
#define mp_hal_pin_obj_t gpio_num_t
mp_hal_pin_obj_t machine_pin_get_id(mp_obj_t pin_in);
#define mp_hal_get_pin_obj(o) machine_pin_get_id(o)
#define mp_hal_pin_name(p) (p)
static inline void mp_hal_pin_input(mp_hal_pin_obj_t pin) {
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
}
static inline void mp_hal_pin_output(mp_hal_pin_obj_t pin) {
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
}
static inline void mp_hal_pin_open_drain(mp_hal_pin_obj_t pin) {
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT_OD);
}
static inline void mp_hal_pin_od_low(mp_hal_pin_obj_t pin) {
    gpio_set_level(pin, 0);
}
static inline void mp_hal_pin_od_high(mp_hal_pin_obj_t pin) {
    gpio_set_level(pin, 1);
}
static inline int mp_hal_pin_read(mp_hal_pin_obj_t pin) {
    return gpio_get_level(pin);
}
static inline int mp_hal_pin_read_output(mp_hal_pin_obj_t pin) {
    return pin < 32
        ? (*(uint32_t *)GPIO_OUT_REG >> pin) & 1
        : (*(uint32_t *)GPIO_OUT1_REG >> (pin - 32)) & 1;
}
static inline void mp_hal_pin_write(mp_hal_pin_obj_t pin, int v) {
    gpio_set_level(pin, v);
}
