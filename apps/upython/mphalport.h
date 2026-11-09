/*
 * MACJr HAL for the MicroPython embed port.
 */
#pragma once

/* Required by extmod/virtpin.h avoidance. */
#define mp_hal_pin_obj_t void *

/* sys_millis() is the tick source. Declared here rather than taken from
 * macjr.h, which pulls in the firmware headers: the qstr step of
 * Makefile.embed preprocesses this file on the host, without them. */
unsigned sys_millis(void);
void sys_delay(unsigned ms);
static inline mp_uint_t mp_hal_ticks_ms(void) { return (mp_uint_t)sys_millis(); }
static inline void      mp_hal_delay_ms(mp_uint_t ms) { sys_delay(ms); }

/* Microseconds for time.ticks_us(), from esp_timer (exported). sleep_us()
 * rounds up to whole milliseconds on sys_delay(), which ends at a kill. */
#include <stdint.h>
int64_t esp_timer_get_time(void);
static inline mp_uint_t mp_hal_ticks_us(void) { return (mp_uint_t)esp_timer_get_time(); }
static inline mp_uint_t mp_hal_ticks_cpu(void) { return mp_hal_ticks_us(); }
static inline void      mp_hal_delay_us(mp_uint_t us) { sys_delay((us + 999) / 1000); }
