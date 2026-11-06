/*
 * Symbolic console implementation: a character-cell grid plus a small
 * ANSI/VT100 subset parser (cursor movement, SGR colors, erase, DEC private
 * modes for cursor visibility). See tty/vc.h for the data model.
 */
#include "tty/vc.h"

#include <string.h>

/* ANSI escape-sequence parser states, driven byte-by-byte in macjr_vc_write(). */
enum {
  ST_NORMAL = 0, /* plain text */
  ST_ESC,        /* just saw 0x1b, waiting to see if it's a CSI ('[') or other */
  ST_CSI,        /* inside "\x1b[...", accumulating numeric params until a final byte */
};

/* Mark row r as needing a redraw by setting its bit in the row_dirty bitmap
 * (one bit per row, 32 rows per uint32_t word). row_dirty may be NULL. */
static void set_row_dirty(macjr_vc_t *vc, uint32_t *row_dirty, int r) {
  if (!row_dirty || r < 0 || r >= vc->rows) {
    return;
  }
  row_dirty[r >> 5] |= (uint32_t)1u << (r & 31);
}

/* Pull cursor_r/cursor_c back into [0, rows) x [0, cols) after arithmetic
 * (escape-sequence cursor moves) that could have pushed them out of bounds. */
static void clamp_cursor(macjr_vc_t *vc) {
  if (vc->cursor_r < 0) {
    vc->cursor_r = 0;
  }
  if (vc->cursor_c < 0) {
    vc->cursor_c = 0;
  }
  if (vc->cursor_r >= vc->rows) {
    vc->cursor_r = vc->rows - 1;
  }
  if (vc->cursor_c >= vc->cols) {
    vc->cursor_c = vc->cols - 1;
  }
}

/* Shift every row up by one (dropping row 0) and blank the new last row.
 * Called when the cursor advances past the last row. */
static void scroll_up(macjr_vc_t *vc, uint32_t *row_dirty) {
  if (vc->rows < 2) {
    return;
  }
  memmove(vc->cells, vc->cells + vc->cols, (size_t)(vc->rows - 1) * sizeof(macjr_cell_t) * (size_t)vc->cols);
  for (int c = 0; c < vc->cols; c++) {
    macjr_cell_t *cell = &vc->cells[(vc->rows - 1) * vc->cols + c];
    cell->ch = ' ';
    cell->fg = vc->fg;
    cell->bg = vc->bg;
  }
  if (row_dirty) {
    for (int r = 0; r < vc->rows; r++) {
      set_row_dirty(vc, row_dirty, r);
    }
  }
}

void macjr_vc_init(macjr_vc_t *vc, int rows, int cols, macjr_cell_t *storage) {
  memset(vc, 0, sizeof(*vc));
  vc->rows = rows;
  vc->cols = cols;
  vc->cells = storage;
  vc->cursor_visible = true;
  vc->fg = 7;
  vc->bg = 0;
  macjr_vc_clear_screen(vc);
}

void macjr_vc_reset_attrs(macjr_vc_t *vc) {
  vc->fg = 7;
  vc->bg = 0;
}

void macjr_vc_clear_screen(macjr_vc_t *vc) {
  for (int i = 0; i < vc->rows * vc->cols; i++) {
    vc->cells[i].ch = ' ';
    vc->cells[i].fg = vc->fg;
    vc->cells[i].bg = vc->bg;
  }
  vc->cursor_r = 0;
  vc->cursor_c = 0;
}

void macjr_vc_mark_all_dirty(macjr_vc_t *vc) {
  (void)vc;
}

/* Write one glyph (with the current SGR colors) into the cell under the
 * cursor and flag that row dirty. Does not move the cursor. */
static void putc_at_cursor(macjr_vc_t *vc, uint32_t *row_dirty, uint16_t ch) {
  int idx = vc->cursor_r * vc->cols + vc->cursor_c;
  macjr_cell_t *cell = &vc->cells[idx];
  cell->ch = ch;
  cell->fg = vc->fg;
  cell->bg = vc->bg;
  set_row_dirty(vc, row_dirty, vc->cursor_r);
}

/* Move the cursor right one cell, wrapping to the next row (and scrolling
 * if that was the last row) like a terminal does after printing a glyph. */
static void advance_cursor(macjr_vc_t *vc, uint32_t *row_dirty) {
  vc->cursor_c++;
  if (vc->cursor_c >= vc->cols) {
    vc->cursor_c = 0;
    vc->cursor_r++;
    if (vc->cursor_r >= vc->rows) {
      vc->cursor_r = vc->rows - 1;
      scroll_up(vc, row_dirty);
    }
  }
}

/* Clear CSI parameter accumulator state; called when entering a fresh
 * "\x1b[" sequence and again once it's fully dispatched. */
static void ansi_reset_params(macjr_vc_t *vc) {
  vc->ansi_param_count = 0;
  vc->ansi_param_acc = 0;
  vc->ansi_private = false;
  for (int i = 0; i < 8; i++) {
    vc->ansi_param[i] = 0;
  }
}

/* Commit the digits accumulated so far as the next CSI parameter (called on
 * each ';' separator and once more for the final parameter before dispatch). */
static void ansi_push_param(macjr_vc_t *vc) {
  if (vc->ansi_param_count < 8) {
    vc->ansi_param[vc->ansi_param_count++] = vc->ansi_param_acc;
  }
  vc->ansi_param_acc = 0;
}

/* Fetch CSI parameter `idx`, returning `def` if it was omitted or defaulted
 * to 0 (matches ANSI convention, e.g. "\x1b[H" == "\x1b[1;1H"). */
static int ansi_get(macjr_vc_t *vc, int idx, int def) {
  if (idx >= vc->ansi_param_count || vc->ansi_param[idx] == 0) {
    return def;
  }
  return vc->ansi_param[idx];
}

/* Apply a Select Graphic Rendition ("\x1b[...m") sequence: 0 resets, 1 maps
 * to the bright variant of the current fg, 30-37/40-47 set base fg/bg, and
 * 90-97/100-107 set the bright (aixterm) fg/bg. */
static void apply_sgr(macjr_vc_t *vc) {
  if (vc->ansi_param_count == 0) {
    macjr_vc_reset_attrs(vc);
    return;
  }
  for (int i = 0; i < vc->ansi_param_count; i++) {
    int n = vc->ansi_param[i];
    if (n == 0) {
      macjr_vc_reset_attrs(vc);
    } else if (n == 1) {
      /* bold: map to bright fg by +8 if base color */
      if (vc->fg < 8) {
        vc->fg = (uint8_t)(vc->fg + 8);
      }
    } else if (n >= 30 && n <= 37) {
      vc->fg = (uint8_t)(n - 30);
    } else if (n >= 40 && n <= 47) {
      vc->bg = (uint8_t)(n - 40);
    } else if (n >= 90 && n <= 97) {
      vc->fg = (uint8_t)(8 + (n - 90));
    } else if (n >= 100 && n <= 107) {
      vc->bg = (uint8_t)(8 + (n - 100));
    }
  }
}

/* Execute one fully-parsed CSI sequence once its final byte (`cmd`) has been
 * read; vc->ansi_param[] holds the sequence's numeric arguments. */
static void csi_dispatch(macjr_vc_t *vc, uint8_t cmd, uint32_t *row_dirty) {
  switch (cmd) {
    case 'm':
      apply_sgr(vc);
      break;
    case 'H': /* CUP: cursor position */
    case 'f': { /* HVP: alias for CUP */
      int row = ansi_get(vc, 0, 1) - 1;
      int col = ansi_get(vc, 1, 1) - 1;
      vc->cursor_r = row;
      vc->cursor_c = col;
      clamp_cursor(vc);
      break;
    }
    case 'J': { /* ED: erase in display. 0=cursor->end (default), 1=start->cursor, 2=all */
      int mode = ansi_get(vc, 0, 0);
      if (mode == 2) {
        macjr_vc_clear_screen(vc);
        if (row_dirty) {
          for (int r = 0; r < vc->rows; r++) {
            set_row_dirty(vc, row_dirty, r);
          }
        }
      } else if (mode == 1) {
        for (int r = 0; r <= vc->cursor_r; r++) {
          int start_c = 0;
          int end_c = (r == vc->cursor_r) ? vc->cursor_c : (vc->cols - 1);
          for (int c = start_c; c <= end_c; c++) {
            macjr_cell_t *cell = &vc->cells[r * vc->cols + c];
            cell->ch = ' ';
            cell->fg = vc->fg;
            cell->bg = vc->bg;
          }
          set_row_dirty(vc, row_dirty, r);
        }
      } else {
        for (int r = vc->cursor_r; r < vc->rows; r++) {
          int start_c = (r == vc->cursor_r) ? vc->cursor_c : 0;
          for (int c = start_c; c < vc->cols; c++) {
            macjr_cell_t *cell = &vc->cells[r * vc->cols + c];
            cell->ch = ' ';
            cell->fg = vc->fg;
            cell->bg = vc->bg;
          }
          set_row_dirty(vc, row_dirty, r);
        }
      }
      break;
    }
    case 'K': { /* EL: erase in line. 0=cursor->end (default), 1=start->cursor, 2=whole line */
      int mode = ansi_get(vc, 0, 0);
      int r = vc->cursor_r;
      if (mode == 1) {
        for (int c = 0; c <= vc->cursor_c; c++) {
          macjr_cell_t *cell = &vc->cells[r * vc->cols + c];
          cell->ch = ' ';
          cell->fg = vc->fg;
          cell->bg = vc->bg;
        }
      } else if (mode == 2) {
        for (int c = 0; c < vc->cols; c++) {
          macjr_cell_t *cell = &vc->cells[r * vc->cols + c];
          cell->ch = ' ';
          cell->fg = vc->fg;
          cell->bg = vc->bg;
        }
      } else {
        for (int c = vc->cursor_c; c < vc->cols; c++) {
          macjr_cell_t *cell = &vc->cells[r * vc->cols + c];
          cell->ch = ' ';
          cell->fg = vc->fg;
          cell->bg = vc->bg;
        }
      }
      set_row_dirty(vc, row_dirty, r);
      break;
    }
    case 'A': /* CUU: cursor up */
      vc->cursor_r -= ansi_get(vc, 0, 1);
      clamp_cursor(vc);
      break;
    case 'B': /* CUD: cursor down */
      vc->cursor_r += ansi_get(vc, 0, 1);
      clamp_cursor(vc);
      break;
    case 'C': /* CUF: cursor forward */
      vc->cursor_c += ansi_get(vc, 0, 1);
      clamp_cursor(vc);
      break;
    case 'D': /* CUB: cursor back */
      vc->cursor_c -= ansi_get(vc, 0, 1);
      clamp_cursor(vc);
      break;
    case 'h': /* DECSET */
    case 'l': /* DECRST -- only mode 25 (text cursor enable) is supported */
      if (vc->ansi_private) {
        int n = ansi_get(vc, 0, 0);
        if (n == 25) {
          vc->cursor_visible = (cmd == 'h');
        }
      }
      break;
    default:
      break;
  }
}

/*
 * Feed a byte stream through the console: a byte-at-a-time state machine
 * that interprets the ANSI subset above and otherwise treats bytes as
 * either control characters (CR/LF/BS/TAB) or printable glyphs.
 */
void macjr_vc_write(macjr_vc_t *vc, const char *buf, size_t len, uint32_t *row_dirty) {
  for (size_t i = 0; i < len; i++) {
    uint8_t c = (uint8_t)buf[i];

    if (vc->ansi_state == ST_CSI) {
      /* Inside "\x1b[...": digits build up the current parameter, ';'
       * separates parameters, '?' flags a DEC-private sequence, and any
       * other byte is the final byte that ends the sequence and dispatches it. */
      if (c >= '0' && c <= '9') {
        vc->ansi_param_acc = vc->ansi_param_acc * 10 + (c - '0');
      } else if (c == ';') {
        ansi_push_param(vc);
      } else if (c == '?') {
        vc->ansi_private = true;
      } else {
        ansi_push_param(vc);
        csi_dispatch(vc, c, row_dirty);
        vc->ansi_state = ST_NORMAL;
        ansi_reset_params(vc);
      }
      continue;
    }

    if (vc->ansi_state == ST_ESC) {
      /* Just saw 0x1b: '[' starts a CSI sequence, 'c' is RIS (full reset),
       * anything else is an escape we don't support -- drop back to normal. */
      if (c == '[') {
        vc->ansi_state = ST_CSI;
        ansi_reset_params(vc);
      } else if (c == 'c') {
        macjr_vc_clear_screen(vc);
        macjr_vc_reset_attrs(vc);
        if (row_dirty) {
          for (int r = 0; r < vc->rows; r++) {
            set_row_dirty(vc, row_dirty, r);
          }
        }
        vc->ansi_state = ST_NORMAL;
      } else {
        vc->ansi_state = ST_NORMAL;
      }
      continue;
    }

    /* ST_NORMAL: plain text plus the handful of C0 control codes a
     * terminal is expected to act on directly (CR, LF, BS, TAB). */
    if (c == 0x1b) {
      vc->ansi_state = ST_ESC;
      continue;
    }

    if (c == '\r') {
      vc->cursor_c = 0;
      continue;
    }
    if (c == '\n') {
      vc->cursor_r++;
      if (vc->cursor_r >= vc->rows) {
        vc->cursor_r = vc->rows - 1;
        scroll_up(vc, row_dirty);
      }
      continue;
    }
    if (c == '\b') {
      if (vc->cursor_c > 0) {
        vc->cursor_c--;
      }
      continue;
    }
    if (c == '\t') {
      /* Advance to the next 8-column tab stop, clamped to the last column. */
      int tab = 8;
      int next = (vc->cursor_c + tab) / tab * tab;
      if (next >= vc->cols) {
        next = vc->cols - 1;
      }
      vc->cursor_c = next;
      continue;
    }

    /* Printable glyph (ASCII 32-126, 128-255 from the pseudographics
     * page, or the arrows at 30 and 31); 127 (DEL) is intentionally not
     * rendered. */
    if (c >= 30 && c != 127) {
      putc_at_cursor(vc, row_dirty, c);
      advance_cursor(vc, row_dirty);
    }
  }
}
