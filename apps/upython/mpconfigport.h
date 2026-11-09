/*
 * MicroPython port configuration for MACJr (ESP32-S3, Xtensa LX7).
 * Loaded as an ELF from /BIN on the SD card.  I/O goes through the MacJr
 * TTY and MK-52 keyboard; errno-dependent modules are disabled.
 */
#pragma once

#include <stdint.h>
#include <alloca.h>

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

/* Keep binary size reasonable. */
#define MICROPY_PY_BUILTINS_STR_UNICODE (0)
#define MICROPY_ERROR_REPORTING         (MICROPY_ERROR_REPORTING_TERSE)

/* sys.platform and the banner's machine part (sys.implementation._machine). */
#define MICROPY_PY_SYS_PLATFORM         "macjr"
#define MICROPY_HW_BOARD_NAME           "MACJr"
#define MICROPY_HW_MCU_NAME             "ESP32-S3"

/* Machine type: 32-bit little-endian. */
typedef long mp_off_t;
