/*
 * TTY lifecycle and the process-facing read/write/flush API. Glues together
 * the symbolic console (vc), the line discipline (ldisc, in ldisc_ntty.c),
 * and the HAL blitter; see tty_internal.h for the shared struct layout.
 */
#include "tty_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "tty/hal_blit.h"

/* Default termios: like a typical Linux tty in cooked mode -- ICRNL on
 * input, ONLCR on output, ICANON+ECHO+ECHOE+ISIG, ^C/^\/DEL/^D as the
 * usual control characters, VMIN=1 (read blocks for at least one byte). */
static void termios_default(struct macjr_termios *t) {
  memset(t, 0, sizeof(*t));
  t->c_iflag = MACJR_ICRNL;
  t->c_oflag = MACJR_OPOST | MACJR_ONLCR;
  t->c_lflag = MACJR_ISIG | MACJR_ICANON | MACJR_ECHO | MACJR_ECHOE;
  t->c_cc[MACJR_VINTR] = 0x03;   /* ^C */
  t->c_cc[MACJR_VQUIT] = 0x1c;   /* ^\ */
  t->c_cc[MACJR_VERASE] = 0x7f;  /* DEL */
  t->c_cc[MACJR_VEOF] = 0x04;    /* ^D */
  t->c_cc[MACJR_VMIN] = 1;
  t->c_cc[MACJR_VTIME] = 0;
}

/* Output post-processing (Linux termios "oflag" stage): expand LF to CRLF
 * when ONLCR is set, then hand the result to the console parser. Batches
 * through a small stack buffer so multi-KB writes don't need a heap alloc. */
void macjr_tty_emit(macjr_tty_t *tty, const uint8_t *data, size_t len) {
  if (!tty || !data || len == 0) {
    return;
  }

  xSemaphoreTake(tty->vc_lock, portMAX_DELAY);

  /* Tiny stack buffer expansion for LF -> CRLF */
  uint8_t tmp[256];

  if (tty->term.c_oflag & MACJR_OPOST) {
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
      uint8_t c = data[i];
      if ((tty->term.c_oflag & MACJR_ONLCR) && c == '\n') {
        if (j + 2 > sizeof(tmp)) {
          macjr_vc_write(&tty->vc, (const char *)tmp, j, tty->row_dirty);
          j = 0;
        }
        tmp[j++] = '\r';
        tmp[j++] = '\n';
      } else {
        if (j + 1 > sizeof(tmp)) {
          macjr_vc_write(&tty->vc, (const char *)tmp, j, tty->row_dirty);
          j = 0;
        }
        tmp[j++] = c;
      }
    }
    if (j > 0) {
      macjr_vc_write(&tty->vc, (const char *)tmp, j, tty->row_dirty);
    }
  } else {
    macjr_vc_write(&tty->vc, (const char *)data, len, tty->row_dirty);
  }

  xSemaphoreGive(tty->vc_lock);
}

ssize_t macjr_tty_write(macjr_tty_t *tty, const void *buf, size_t len) {
  if (!tty || !buf) {
    return -1;
  }
  /* User -> driver path mirrors Linux: output post-processing then console. */
  macjr_tty_emit(tty, (const uint8_t *)buf, len);
  return (ssize_t)len;
}

/* User-facing read(): delegates to the line discipline, which blocks for a
 * full line in canonical mode or applies VMIN/VTIME semantics in raw mode. */
ssize_t macjr_tty_read(macjr_tty_t *tty, void *buf, size_t len) {
  if (!tty || !buf) {
    return -1;
  }
  return macjr_ldisc_read(&tty->ldisc, (uint8_t *)buf, len);
}

/* Keyboard-driver entry point: feed raw input bytes into the line discipline. */
void macjr_kbd_inject(macjr_tty_t *tty, const uint8_t *data, size_t len) {
  if (!tty || !data || len == 0) {
    return;
  }
  macjr_ldisc_rx(&tty->ldisc, data, len);
}

/* Push every row touched since the last flush to the panel, then clear the
 * dirty bitmap. Always re-marks the cursor's row dirty first so the cursor
 * (drawn as part of the blit) stays visually in sync even if nothing else
 * on that row changed. */
void macjr_display_flush(macjr_tty_t *tty) {
  if (!tty || !tty->hal) {
    return;
  }
  xSemaphoreTake(tty->vc_lock, portMAX_DELAY);
  if (tty->vc.cursor_r >= 0 && tty->vc.cursor_r < tty->vc.rows) {
    int cr = tty->vc.cursor_r;
    tty->row_dirty[cr >> 5] |= (1u << (cr & 31));
  }
  macjr_hal_blit_vc(tty->hal, &tty->vc, tty->row_dirty, tty->glyph_w, tty->glyph_h, tty->row_dirty_words);
  for (int i = 0; i < tty->row_dirty_words; i++) {
    tty->row_dirty[i] = 0;
  }
  xSemaphoreGive(tty->vc_lock);
}

/* Allocate a tty sized to fill hal's pixel surface with glyph_w x glyph_h
 * cells, then bring up the console/termios/line-discipline state. Every
 * allocation/RTOS-object failure unwinds everything allocated so far before
 * returning NULL -- there's no partial/leaked tty on the failure path. */
macjr_tty_t *macjr_tty_create(macjr_hal_display_t *hal, int glyph_w, int glyph_h) {
  if (!hal || glyph_w <= 0 || glyph_h <= 0) {
    return NULL;
  }

  macjr_tty_t *tty = (macjr_tty_t *)calloc(1, sizeof(*tty));
  if (!tty) {
    return NULL;
  }

  tty->hal = hal;
  tty->glyph_w = glyph_w;
  tty->glyph_h = glyph_h;

  int cols = hal->width_px / glyph_w;
  int rows = hal->height_px / glyph_h;
  if (cols < 1 || rows < 1) {
    free(tty);
    return NULL;
  }

  tty->cell_storage = (macjr_cell_t *)calloc((size_t)rows * (size_t)cols, sizeof(macjr_cell_t));
  if (!tty->cell_storage) {
    free(tty);
    return NULL;
  }

  /* One dirty bit per row, packed 32 to a word. */
  tty->row_dirty_words = (rows + 31) / 32;
  tty->row_dirty = (uint32_t *)calloc((size_t)tty->row_dirty_words, sizeof(uint32_t));
  if (!tty->row_dirty) {
    free(tty->cell_storage);
    free(tty);
    return NULL;
  }

  tty->vc_lock = xSemaphoreCreateMutex();
  tty->ldisc_lock = xSemaphoreCreateMutex();
  tty->line_sem = xSemaphoreCreateBinary();
  tty->raw_q = xQueueCreate(MACJR_RAW_QUEUE_LEN, sizeof(uint8_t));

  if (!tty->vc_lock || !tty->ldisc_lock || !tty->line_sem || !tty->raw_q) {
    /* Tear down whichever of the four RTOS objects above did get created. */
    if (tty->raw_q) {
      vQueueDelete(tty->raw_q);
    }
    if (tty->line_sem) {
      vSemaphoreDelete(tty->line_sem);
    }
    if (tty->ldisc_lock) {
      vSemaphoreDelete(tty->ldisc_lock);
    }
    if (tty->vc_lock) {
      vSemaphoreDelete(tty->vc_lock);
    }
    free(tty->row_dirty);
    free(tty->cell_storage);
    free(tty);
    return NULL;
  }

  macjr_vc_init(&tty->vc, rows, cols, tty->cell_storage);
  termios_default(&tty->term);
  tty->ws.ws_row = (uint16_t)rows;
  tty->ws.ws_col = (uint16_t)cols;
  tty->ws.ws_xpixel = (uint16_t)hal->width_px;
  tty->ws.ws_ypixel = (uint16_t)hal->height_px;

  macjr_ldisc_init(&tty->ldisc, tty);

  /* Force a full redraw on the first flush, since the panel's actual
   * contents (if any) are unknown at this point. */
  memset(tty->row_dirty, 0xFF, (size_t)tty->row_dirty_words * sizeof(uint32_t));

  return tty;
}

void macjr_tty_destroy(macjr_tty_t *tty) {
  if (!tty) {
    return;
  }
  if (tty->raw_q) {
    vQueueDelete(tty->raw_q);
  }
  if (tty->line_sem) {
    vSemaphoreDelete(tty->line_sem);
  }
  if (tty->ldisc_lock) {
    vSemaphoreDelete(tty->ldisc_lock);
  }
  if (tty->vc_lock) {
    vSemaphoreDelete(tty->vc_lock);
  }
  free(tty->row_dirty);
  free(tty->cell_storage);
  free(tty);
}

macjr_vc_t *macjr_tty_vc(macjr_tty_t *tty) {
  return tty ? &tty->vc : NULL;
}

struct macjr_termios *macjr_tty_termios(macjr_tty_t *tty) {
  return tty ? &tty->term : NULL;
}

struct macjr_winsize *macjr_tty_winsize(macjr_tty_t *tty) {
  return tty ? &tty->ws : NULL;
}
