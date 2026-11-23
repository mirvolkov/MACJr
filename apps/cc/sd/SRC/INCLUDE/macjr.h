/*
 * macjr.h -- the MACJr API for programs built on the board with cc.
 *
 * A program's entry point gets the screen as a third argument:
 *
 *   int main(int argc, char *argv[], macjr_tty_t *tty);
 *
 * The screen is a 40x15 text console with ANSI escapes and CP866 glyphs.
 * Write to it with macjr_tty_write() and show it with macjr_display_flush(),
 * or with printf() (stdio.h), which goes to the same screen and shows at
 * once. The screen is also the file /dev/tty and descriptors 0..2.
 * F and Cx together kill a program.
 */
#ifndef MACJR_H
#define MACJR_H

#include <stddef.h>

typedef struct macjr_tty macjr_tty_t;

/* the screen */
int macjr_tty_write(macjr_tty_t *tty, const void *buf, size_t len);
void macjr_display_flush(macjr_tty_t *tty);
void macjr_tty_header(macjr_tty_t *tty, const char *title); /* ════[title]════ on row 1 */

/* the system */
int sys_getkey(void);         /* wait for a key, return its code (below) */
unsigned sys_millis(void);    /* milliseconds since boot */
void sys_delay(unsigned ms);
void sys_exit(int status);

/* keys: the MK-52 keyboard */
#define KEY_ESC      27   /* Cx: "back" */
#define KEY_ENTER    148  /* В↑ */
#define KEY_0        132
#define KEY_1        134
#define KEY_2        133
#define KEY_3        139
#define KEY_4        137
#define KEY_5        135
#define KEY_6        136
#define KEY_7        131
#define KEY_8        130
#define KEY_9        138
#define KEY_DOT      145  /* . */
#define KEY_NEG      146  /* /-/ */
#define KEY_EE       141  /* ВП */
#define KEY_PLUS     142
#define KEY_MINUS    144
#define KEY_MUL      143
#define KEY_DIV      149
#define KEY_SWAP     147  /* ↔ */
#define KEY_UP       156  /* П→x */
#define KEY_DOWN     157  /* x→П */
#define KEY_LEFT     151  /* ←ШГ */
#define KEY_RIGHT    155  /* ШГ→ */
#define KEY_SP       152  /* С/П */
#define KEY_BP       154  /* БП */
#define KEY_PP       159  /* ПП */
#define KEY_VO       153  /* В/О */

#endif
