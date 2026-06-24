/*
 * Console-to-pixel blitter: walks the dirty rows of a symbolic console and
 * draws each cell's background plus glyph bitmap through the display HAL.
 */
#include "tty/hal_blit.h"

#include <stdint.h>

extern const uint8_t macjr_font_builtin_8x16[256][16];

/* Standard 16-color ANSI palette (8 base + 8 bright), as RGB888. */
static uint32_t ansi16(uint8_t idx) {
  static const uint32_t pal[16] = {
      0x000000, 0xaa0000, 0x00aa00, 0xaaaa00, 0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
      0x555555, 0xff5555, 0x55ff55, 0xffff55, 0x5555ff, 0xff55ff, 0x55ffff, 0xffffff,
  };
  return pal[idx & 15u];
}

void macjr_hal_blit_vc(macjr_hal_display_t *hal, macjr_vc_t *vc, uint32_t *row_dirty, int glyph_w, int glyph_h,
                       int row_dirty_words) {
  if (!hal || !vc || !vc->cells) {
    return;
  }

  if (hal->begin_frame) {
    hal->begin_frame(hal);
  }

  for (int r = 0; r < vc->rows; r++) {
    /* Skip rows that aren't flagged dirty -- this is the whole point of
     * the dirty bitmap: avoid redrawing unchanged rows over (slow) SPI. */
    int widx = r >> 5;
    int bit = r & 31;
    if (widx < 0 || widx >= row_dirty_words) {
      continue;
    }
    if (!row_dirty || ((row_dirty[widx] >> bit) & 1u) == 0u) {
      continue;
    }

    for (int c = 0; c < vc->cols; c++) {
      const macjr_cell_t *cell = &vc->cells[r * vc->cols + c];
      uint16_t ch = cell->ch;
      if (ch > 255) {
        ch = '?';
      }
      const uint8_t *bitmap = macjr_font_builtin_8x16[ch];

      /* Center the fixed 8x16 glyph within a (possibly larger) cell. */
      int x = c * glyph_w + (glyph_w > 8 ? (glyph_w - 8) / 2 : 0);
      int y = r * glyph_h + (glyph_h > 16 ? (glyph_h - 16) / 2 : 0);

      uint32_t bg = ansi16(cell->bg);
      uint32_t fg = ansi16(cell->fg);

      if (hal->fill_rect) {
        hal->fill_rect(hal, c * glyph_w, r * glyph_h, glyph_w, glyph_h, bg);
      }
      if (hal->blit_1bpp_mask) {
        hal->blit_1bpp_mask(hal, x, y, 8, 16, 1, bitmap, fg, bg);
      }
    }
  }

  /* Draw the cursor as a 2px-tall white underline at the bottom of its
   * cell, on top of whatever glyph/background was just drawn there. */
  if (vc->cursor_visible) {
    int cr = vc->cursor_r;
    int cc = vc->cursor_c;
    if (cr >= 0 && cr < vc->rows && cc >= 0 && cc < vc->cols) {
      uint32_t inv = 0xffffff;
      if (hal->fill_rect) {
        int cell_x = cc * glyph_w;
        int cell_y = cr * glyph_h;
        hal->fill_rect(hal, cell_x, cell_y + glyph_h - 2, glyph_w, 2, inv);
      }
    }
  }

  if (hal->end_frame) {
    hal->end_frame(hal);
  }
}
