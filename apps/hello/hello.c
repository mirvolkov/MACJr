/*
 * HELLO.ELF -- the first MacJr program loaded from the SD card.
 *
 * Shows its arguments and where the loader put it (code runs from the
 * PSRAM instruction bus alias at 0x42..., data sits at 0x3C...), then
 * echoes the MK-52 labels of the keys pressed until Cx and returns how
 * many keys it got.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macjr.h"

static int s_runs; /* .bss: zero again on every load */
static macjr_tty_t *s_tty;

/* printf() to the screen: format, write to the tty, show it. */
static void tprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tprintf(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    macjr_tty_write(s_tty, buf, n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    macjr_display_flush(s_tty);
  }
}

/* MK-52 key labels by code (128-159), Cyrillic in CP866 written as \x
 * escapes. The font has no arrows, × or ÷: ^ v < > * / stand in. */
static const char *const s_key_labels[32] = {
  "^v",               /* 128 ↑↓  */
  "\x80^",            /* 129 А↑  */
  "8", "7", "0", "2", "1", "5", "6", "4", "9", "3", /* 130-139 */
  "\x91x",            /* 140 Сx  */
  "\x82\x8F",         /* 141 ВП  */
  "+",                /* 142 +   */
  "*",                /* 143 ×   */
  "-",                /* 144 −   */
  ".",                /* 145 .   */
  "/-/",              /* 146 /-/ */
  "<>",               /* 147 ↔   */
  "\x82^",            /* 148 В↑  */
  "/",                /* 149 ÷   */
  "\x8A",             /* 150 К   */
  "<\x98\x83",        /* 151 ←ШГ */
  "\x91/\x8F",        /* 152 С/П */
  "\x82/\x8E",        /* 153 В/О */
  "\x81\x8F",         /* 154 БП  */
  "\x98\x83>",        /* 155 ШГ→ */
  "\x8F>x",           /* 156 П→x */
  "x>\x8F",           /* 157 x→П */
  "F",                /* 158 F   */
  "\x8F\x8F",         /* 159 ПП  */
};

static void print_key(int key) {
  if (key >= 128 && key < 160) {
    tprintf("%s ", s_key_labels[key - 128]);
  } else {
    tprintf("%d ", key); /* not an MK-52 key */
  }
}

int main(int argc, char *argv[], macjr_tty_t *tty) {
  unsigned start = sys_millis();
  s_tty = tty;
  s_runs++;

  tprintf("\x1b[2J\x1b[H\x1b[?25l");
  macjr_tty_header(tty, "Hello from the SD card!");

  tprintf("\x1b[96margc   \x1b[97m%d\r\n", argc);
  for (int i = 0; i < argc; i++) {
    tprintf("\x1b[96margv[%d]\x1b[97m %s\r\n", i, argv[i]);
  }

  char *heap = malloc(64);
  tprintf("\x1b[96mcode   \x1b[97m%p\r\n", (void *)main);
  tprintf("\x1b[96mdata   \x1b[97m%p\r\n", (void *)&s_runs);
  tprintf("\x1b[96mheap   \x1b[97m%p\r\n", (void *)heap);
  /* one line: the screen has 15 rows and the loader adds two at the end */
  tprintf("\x1b[96mruns   \x1b[97m%d   \x1b[96mcpu \x1b[97m%d\r\n", s_runs, sys_cpu());
  free(heap);

  tprintf("\x1b[90mKeys echo here, Cx ends\x1b[0m\r\n");
  int keys = 0;
  for (;;) {
    int key = sys_getkey();
    if (key == SYS_KEY_ESC) {
      break;
    }
    keys++;
    print_key(key);
  }

  tprintf("\r\nran %u ms", sys_millis() - start);
  return keys;
}
