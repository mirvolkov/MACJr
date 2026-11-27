/*
 * MicroPython port configuration for MACJr (ESP32-S3, Xtensa LX7).
 * Loaded as an ELF from /bin (the board's flash).  I/O goes through the MacJr
 * TTY and MK-52 keyboard; errno-dependent modules are disabled.
 */
#pragma once

#include <stdint.h>
#include <alloca.h>
/* ESP_IDF_VERSION, which the esp32 port's machine code tests */
#include "esp_idf_version.h"
/* ESP-IDF 6 dropped the old name of the 12 dB ADC attenuation (it was an
 * alias, same value), which the esp32 port's ADC code still uses. */
#if ESP_IDF_VERSION_MAJOR >= 6
#define ADC_ATTEN_DB_11 ADC_ATTEN_DB_12
#endif

/* Our HAL header (mphalport.h lives next to this file). */
#define MICROPY_MPHALPORT_H "mphalport.h"

/* Basic Python: lists, dicts, comprehensions, exceptions, math. */
#define MICROPY_CONFIG_ROM_LEVEL        (MICROPY_CONFIG_ROM_LEVEL_BASIC_FEATURES)

/* Exceptions (nlr) by setjmp/longjmp, as on MicroPython's ESP32 port:
 * the Xtensa assembly nlr is for the call0 ABI (ESP8266), and the S3 uses
 * register windows, so a raise came back with a garbage exception. */
#define MICROPY_NLR_SETJMP              (1)

/* The garbage collector, on the heap main.c gives mp_embed_init(). */
#define MICROPY_ENABLE_GC               (1)
/* Registers for the GC scan come from setjmp(): MicroPython has no
 * register helper for Xtensa, and setjmp spills the register windows. */
#define MICROPY_GCREGS_SETJMP           (1)

/* mp_repl_continue_with_input(): the REPL's "..." continuation lines. */
#define MICROPY_HELPER_REPL             (1)

/* f-strings: f'{x:.4f}' (off at the BASIC level). */
#define MICROPY_PY_FSTRINGS             (1)

/* Integers of any size (2**100, factorials): MicroPython's own MPZ. A
 * result that leaves the small-int range (31 bits) becomes a long int in
 * C; the assembly fast path (vm_xtensa.S) does small ints only and hands
 * such results back. */
#define MICROPY_LONGINT_IMPL            (MICROPY_LONGINT_IMPL_MPZ)

/* Double-precision float — matches the MK-52's 8-digit BCD mantissa. */
#define MICROPY_FLOAT_IMPL              (MICROPY_FLOAT_IMPL_DOUBLE)

/* Stack depth check: main.c sets the limit inside exec's 16 KB stack. */
#define MICROPY_STACK_CHECK             (1)

/* Math module on; erf/gamma need libm symbols not in firmware exports. */
#define MICROPY_PY_MATH                 (1)
#define MICROPY_PY_MATH_SPECIAL_FUNCTIONS (0)
#define MICROPY_PY_CMATH                (0)

/* Disable everything that calls errno or makes OS/file syscalls. */
#define MICROPY_PY_IO                   (0)
#define MICROPY_PY_IO_IOBASE            (0)
#define MICROPY_PY_IO_BYTESIO           (0)
#define MICROPY_PY_IO_BUFFEREDWRITER    (0)
#define MICROPY_PY_OS                   (0)
#define MICROPY_PY_OS_UNAME             (0)
#define MICROPY_PY_TIME                 (1) /* ticks_ms/us, ticks_diff, sleep (mphalport.h) */
#define MICROPY_PY_UERRNO               (0)
#define MICROPY_PY_USELECT              (0)

/* Minimal sys (no path, no stream objects). */
#define MICROPY_PY_SYS                  (1)
#define MICROPY_PY_SYS_PATH             (0)
#define MICROPY_PY_SYS_MODULES          (0)
#define MICROPY_PY_SYS_STDIN            (0)
#define MICROPY_PY_SYS_STDOUT           (0)
#define MICROPY_PY_SYS_STDERR           (0)
#define MICROPY_PY_SYS_EXC_INFO         (0)

/* machine: the ESP32 port's own Pin, PWM (LEDC) and ADC code, and the
 * portable SoftI2C, SoftSPI, Signal, time_pulse_us and mem8/16/32. The
 * settings are the esp32 port's (ports/esp32/mpconfigport.h). */
#define MICROPY_PY_MACHINE              (1)
#define MICROPY_PY_MACHINE_INCLUDEFILE  "modmachine_port.c"
#define MICROPY_PY_MACHINE_PIN_MAKE_NEW mp_pin_make_new
#define MICROPY_PY_MACHINE_PWM          (1)
#define MICROPY_PY_MACHINE_PWM_DUTY     (1)
#define MICROPY_PY_MACHINE_PWM_INCLUDEFILE "ports/esp32/machine_pwm.c"
#define MICROPY_PY_MACHINE_ADC          (1)
#define MICROPY_PY_MACHINE_ADC_INCLUDEFILE "ports/esp32/machine_adc.c"
#define MICROPY_PY_MACHINE_ADC_ATTEN_WIDTH (1)
#define MICROPY_PY_MACHINE_ADC_INIT     (1)
#define MICROPY_PY_MACHINE_ADC_DEINIT   (1)
#define MICROPY_PY_MACHINE_ADC_READ     (1)
#define MICROPY_PY_MACHINE_ADC_READ_UV  (1)
#define MICROPY_PY_MACHINE_ADC_BLOCK    (1) /* the ADC code's units live here */
#define MICROPY_PY_MACHINE_ADC_BLOCK_INCLUDEFILE "ports/esp32/machine_adc_block.c"
#define MICROPY_PY_MACHINE_SOFTI2C      (1)
#define MICROPY_PY_MACHINE_SOFTSPI      (1)
#define MICROPY_PY_MACHINE_SIGNAL       (1)
#define MICROPY_PY_MACHINE_PULSE        (1)
#define MICROPY_PY_MACHINE_MEMX         (1)
#define MICROPY_PY_MACHINE_PIN_BASE     (1)
/* machine_pin.h's pin table leaves out GPIO 19/20 when the board uses USB
 * (an esp32-port board setting): this one does, for the console and USB. */
#define CONFIG_USB_CDC_ENABLED          (1)

/* Keep binary size reasonable. */
#define MICROPY_PY_BUILTINS_STR_UNICODE (0)
#define MICROPY_ERROR_REPORTING         (MICROPY_ERROR_REPORTING_TERSE)

/* sys.platform and the banner's machine part (sys.implementation._machine). */
#define MICROPY_PY_SYS_PLATFORM         "macjr"
#define MICROPY_HW_BOARD_NAME           "MACJr"
#define MICROPY_HW_MCU_NAME             "ESP32-S3"

/* Machine type: 32-bit little-endian. */
typedef long mp_off_t;
