/* NetHack 3.6	macjrtty.c	MacJr port */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * MACJR: the terminal of the tty port is the board's screen and keys.
 *
 * Output: the game's stdout and stderr (include/macjrio.h) collect into
 * a buffer, with '\n' sent as "\r\n" as a Unix tty does it (ONLCR); the
 * buffer goes to the screen when the game flushes stdout, which the
 * tty port does before it waits for a key, or when it fills up. So a
 * turn's worth of cursor moves and glyphs reaches the panel in one go.
 * Every other FILE goes to the C library unchanged.
 *
 * Input: tgetch() is macjr_getch(): a key from the board, the MK-52
 * keys translated to the characters NetHack expects (see s_mk52); a USB
 * keyboard gives ASCII, which passes through.
 *
 * This file is compiled without hack.h, so the stdio names here are the
 * C library's.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "keyboard.h"
#include "macjr.h"

static macjr_tty_t *s_tty;

/* the handles for stdin, stdout and stderr (macjrio.h); only compared */
static char s_in_tag, s_out_tag;
FILE *const macjr_stdin = (FILE *) &s_in_tag;
FILE *const macjr_stdout = (FILE *) &s_out_tag;

#define OUT_SIZE 2048
static char s_out[OUT_SIZE];
static size_t s_out_len;

void
macjr_tty_attach(macjr_tty_t *tty)
{
    s_tty = tty;
    s_out_len = 0;
}

static void
out_flush(void)
{
    if (s_out_len && s_tty) {
        macjr_tty_write(s_tty, s_out, s_out_len);
        macjr_display_flush(s_tty);
    }
    s_out_len = 0;
}

static void
out_byte(char c)
{
    if (s_out_len + 2 > OUT_SIZE)
        out_flush();
    if (c == '\n')
        s_out[s_out_len++] = '\r';
    s_out[s_out_len++] = c;
}

static void
out_text(const char *s, size_t n)
{
    while (n--)
        out_byte(*s++);
}

int
macjr_fputc(int c, FILE *f)
{
    if (f == macjr_stdout) {
        out_byte((char) c);
        return (unsigned char) c;
    }
    return fputc(c, f);
}

int
macjr_fputs(const char *s, FILE *f)
{
    if (f == macjr_stdout) {
        out_text(s, strlen(s));
        return 0;
    }
    return fputs(s, f);
}

int
macjr_puts(const char *s)
{
    out_text(s, strlen(s));
    out_byte('\n');
    return 0;
}

int
macjr_fflush(FILE *f)
{
    if (f == macjr_stdout) {
        out_flush();
        return 0;
    }
    if (f == macjr_stdin)
        return 0;
    return fflush(f);
}

int
macjr_vfprintf(FILE *f, const char *fmt, va_list ap)
{
    char buf[512];
    int n;

    if (f != macjr_stdout)
        return vfprintf(f, fmt, ap);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0)
        out_text(buf, (size_t) n < sizeof buf ? (size_t) n : sizeof buf - 1);
    return n;
}

int
macjr_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = macjr_vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int
macjr_printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = macjr_vfprintf(macjr_stdout, fmt, ap);
    va_end(ap);
    return n;
}

void
macjr_perror(const char *s)
{
    macjr_fprintf(macjr_stdout, "%s: failed\n", s);
}

void
macjr_clearerr(FILE *f)
{
    if (f != macjr_stdin && f != macjr_stdout)
        clearerr(f);
}

/* The screen size, for the tty port (get_scr_size() in macjrunix.c);
 * smaller than 80x24, so the map is clipped around the hero. */
void
macjr_winsize(int *cols, int *rows)
{
    struct macjr_winsize *ws = s_tty ? macjr_tty_winsize(s_tty) : NULL;

    *cols = ws && ws->ws_col ? ws->ws_col : 40;
    *rows = ws && ws->ws_row ? ws->ws_row : 15;
}

/*
 * MK-52 keys -> NetHack (with the number_pad option, nethackrc):
 *
 *   7 8 9    move; 5 + direction runs, 0 is the inventory
 *   4 5 6
 *   1 2 3
 *   0 .      .  rest one turn
 *   /-/      s  search            ВП   ,  pick up
 *   −        -  fight + direction +    <  go up stairs
 *   ×        >  go down stairs    ÷    :  look here
 *   ↔        space: next page, dismiss
 *   В↑       Enter                Cx   Esc
 *   С/П      y  yes               В/О  n  no
 *
 * Letters (item choices, names) need a USB keyboard for now.
 */
static int
mk52(int code)
{
    switch (code) {
    case KEY_MK_0: return '0';
    case KEY_MK_1: return '1';
    case KEY_MK_2: return '2';
    case KEY_MK_3: return '3';
    case KEY_MK_4: return '4';
    case KEY_MK_5: return '5';
    case KEY_MK_6: return '6';
    case KEY_MK_7: return '7';
    case KEY_MK_8: return '8';
    case KEY_MK_9: return '9';
    case KEY_MK_DOT: return '.';
    case KEY_MK_NEG: return 's';
    case KEY_MK_EE: return ',';
    case KEY_MK_MINUS: return '-';
    case KEY_MK_PLUS: return '<';
    case KEY_MK_MUL: return '>';
    case KEY_MK_DIV: return ':';
    case KEY_MK_SWAP: return ' ';
    case KEY_MK_ENTER: return '\n';
    case KEY_MK_SP: return 'y';
    case KEY_MK_VO: return 'n';
    default: return 0; /* not mapped: wait for another key */
    }
}

int
macjr_getch(void)
{
    out_flush();
    for (;;) {
        int code = sys_getkey(); /* ends the game here on F + Cx */

        if (code == '\r')
            return '\n';
        if (code > 0 && code < 128)
            return code; /* ASCII: Esc (Cx), a USB keyboard */
        code = mk52(code);
        if (code)
            return code;
    }
}
