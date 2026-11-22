#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>

/* Format text for the screen with snprintf() and print it with
 * macjr_tty_write() (macjr.h). printf(), puts() and putchar() go to the
 * serial console, not to the screen. A function of your own taking "..."
 * hands its arguments on with vsnprintf() (stdarg.h). */
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);
int printf(const char *fmt, ...);
int puts(const char *s);
int putchar(int c);

#endif
