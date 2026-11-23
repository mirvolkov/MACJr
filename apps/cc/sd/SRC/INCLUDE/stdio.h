#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>

#define EOF (-1)

/* printf(), vprintf(), puts() and putchar() print on the program's
 * screen: descriptor 1, /dev/tty. ANSI escapes and CP866 work as with
 * macjr_tty_write(), "\n" starts a new line, and each call shows up at
 * once (the screen is flushed). snprintf()/vsnprintf() format into a
 * buffer. A function of your own taking "..." hands its arguments on with
 * vprintf() or vsnprintf() (stdarg.h). */
int printf(const char *fmt, ...);
int vprintf(const char *fmt, __builtin_va_list ap);
int puts(const char *s);
int putchar(int c);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);

#endif
