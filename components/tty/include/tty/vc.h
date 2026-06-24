/*
 * Symbolic console (the "vc" of /dev/ttyN's virtual console, Linux-style):
 * a rows x cols grid of character cells plus a small ANSI/VT100 escape-code
 * interpreter. This layer knows nothing about pixels or fonts -- it just
 * tracks what character and colors belong in each cell, and which rows have
 * changed (row_dirty bitmap) so the HAL blitter only redraws what's needed.
 */
#ifndef MACJR_VC_H
#define MACJR_VC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct macjr_vc macjr_vc_t;

/* One screen cell: a code point plus its 4-bit ANSI fg/bg color indices. */
typedef struct {
  uint16_t ch; /* code point; ASCII 0-127 plus the 128-255 pseudographics page */
  uint8_t fg;  /* 0-15 ANSI foreground */
  uint8_t bg;  /* 0-15 ANSI background */
} macjr_cell_t;

struct macjr_vc {
  int rows;
  int cols;
  macjr_cell_t *cells; /* caller-owned backing store, rows*cols cells, row-major */

  int cursor_r;
  int cursor_c;
  bool cursor_visible;

  uint8_t fg; /* current SGR foreground, applied to newly written cells */
  uint8_t bg; /* current SGR background, applied to newly written cells */
  bool insert_mode;

  /* ANSI escape-sequence parser state (internal, see vc.c's ST_* enum). */
  int ansi_state;
  int ansi_param[8];   /* CSI numeric parameters, e.g. the "1;34" in \x1b[1;34m */
  int ansi_param_count;
  int ansi_param_acc;  /* parameter currently being accumulated digit-by-digit */
  bool ansi_private;   /* set on CSI "?" (private/DEC sequences, e.g. \x1b[?25l) */
};

/* Bind vc to a caller-allocated rows*cols cell array and reset to a blank screen. */
void macjr_vc_init(macjr_vc_t *vc, int rows, int cols, macjr_cell_t *storage);
/* Reset current SGR colors to the default fg/bg (as if "\x1b[0m" was seen). */
void macjr_vc_reset_attrs(macjr_vc_t *vc);
/* Blank every cell to ' ' with the current colors and home the cursor. */
void macjr_vc_clear_screen(macjr_vc_t *vc);
/* Currently a no-op placeholder; callers track full-screen redraws elsewhere. */
void macjr_vc_mark_all_dirty(macjr_vc_t *vc);

/* Write process output / echo stream into the symbolic console (ANSI subset). */
void macjr_vc_write(macjr_vc_t *vc, const char *buf, size_t len, uint32_t *row_dirty);

#endif
