/*
 * Private layout of macjr_tty_t and the line discipline, shared between
 * tty.c (lifecycle, read/write/flush) and ldisc_ntty.c (canonical/raw input
 * state machine). Not part of the public tty/tty.h API.
 */
#pragma once

#include "esp32s3hal/display.h"
#include "tty/termios.h"
#include "tty/vc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MACJR_LINE_CAP 512     /* max bytes buffered for one canonical-mode line */
#define MACJR_RAW_QUEUE_LEN 512 /* depth of the raw-mode byte queue */

typedef struct macjr_tty macjr_tty_t;

/* Termios-like line discipline: buffers/echoes input in canonical mode
 * (building up canon_line until a line terminator) or passes bytes straight
 * through to tty->raw_q in raw mode. See ldisc_ntty.c. */
typedef struct {
  macjr_tty_t *owner;

  uint8_t canon_line[MACJR_LINE_CAP]; /* completed/in-progress canonical line */
  size_t canon_len;

  bool canon_ready;       /* a full line is sitting in canon_line, ready to be read */
  size_t canon_read_len;  /* length already consumed by macjr_ldisc_read() */
} macjr_ldisc_t;

struct macjr_tty {
  macjr_hal_display_t *hal;
  struct macjr_termios term;
  struct macjr_winsize ws;

  macjr_vc_t vc;             /* symbolic character-cell console */
  macjr_cell_t *cell_storage; /* backing array for vc.cells, rows*cols cells */
  uint32_t *row_dirty;        /* bitmap of rows changed since the last flush */
  int row_dirty_words;        /* number of uint32_t words in row_dirty */

  macjr_ldisc_t ldisc;

  SemaphoreHandle_t vc_lock;    /* guards vc + cell_storage + row_dirty */
  SemaphoreHandle_t ldisc_lock; /* guards ldisc state */
  SemaphoreHandle_t line_sem;   /* signaled when a canonical line becomes ready */
  QueueHandle_t raw_q;          /* raw-mode input byte queue */

  int glyph_w;
  int glyph_h;
};

/* Reset l to an empty line bound to owner. */
void macjr_ldisc_init(macjr_ldisc_t *l, macjr_tty_t *owner);
/* Feed bytes typed at the keyboard through the line discipline (echo + buffering). */
void macjr_ldisc_rx(macjr_ldisc_t *l, const uint8_t *data, size_t len);
/* Consume a completed line (canonical mode) or raw bytes (raw mode) for macjr_tty_read(). */
ssize_t macjr_ldisc_read(macjr_ldisc_t *l, uint8_t *buf, size_t len);

/* Write process output bytes into the console, expanding LF to CRLF as needed. */
void macjr_tty_emit(macjr_tty_t *tty, const uint8_t *data, size_t len);
