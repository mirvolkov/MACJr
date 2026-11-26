/* NetHack 3.6	macjrio.h	MacJr port */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * MACJR: the tty port talks to the terminal through stdout (putchar,
 * fputs, fflush, printf...) and tgetch(). On MacJr the terminal is the
 * board's 40x15 screen, so stdout and stderr are replaced here by
 * handles for it, and the stdio calls the game makes on them go to
 * sys/macjr/macjrtty.c: a buffer that reaches the screen on fflush(),
 * which the tty port calls before it waits for a key. The same calls
 * on any other FILE (the data files, the logs) go to the C library.
 *
 * Included last by unixconf.h, after the system headers.
 */
#ifndef MACJRIO_H
#define MACJRIO_H

#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

/* errno lives in each task's thread-local storage, which a program
 * loaded from the card can't reach: here it is a plain variable, which
 * the C library doesn't set (so it reads 0, "no details"). */
#undef errno
extern int macjr_errno;
#define errno macjr_errno

extern FILE *const macjr_stdin, *const macjr_stdout;

#undef stdin
#undef stdout
#undef stderr
#define stdin macjr_stdin
#define stdout macjr_stdout
#define stderr macjr_stdout

int macjr_fputc(int, FILE *);
int macjr_fputs(const char *, FILE *);
int macjr_puts(const char *);
int macjr_fflush(FILE *);
int macjr_fprintf(FILE *, const char *, ...);
int macjr_vfprintf(FILE *, const char *, va_list);
int macjr_printf(const char *, ...);
void macjr_clearerr(FILE *);
void macjr_perror(const char *);
int macjr_getch(void); /* tgetch(): the next key, as NetHack knows them */
void macjr_winsize(int *cols, int *rows);

#undef putc
#undef putchar
#undef clearerr
#define fputc macjr_fputc
#define putc macjr_fputc
#define putchar(c) macjr_fputc((c), stdout)
#define fputs macjr_fputs
#define puts macjr_puts
#define fflush macjr_fflush
#define fprintf macjr_fprintf
#define vfprintf macjr_vfprintf
#define printf macjr_printf
#define clearerr macjr_clearerr
#define setbuf(f, buf) ((void) 0) /* the screen has its own buffer */
#define perror macjr_perror
/* abort() would stop the whole board: end the program instead */
#define abort() exit(EXIT_FAILURE)

#endif /* MACJRIO_H */
