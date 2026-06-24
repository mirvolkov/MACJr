/*
 * Bridges the symbolic console (vc.h) to a pixel HAL display (hal_display.h):
 * renders each dirty character cell as a filled background rect plus an
 * 8x16 glyph from the built-in font, using the cell's ANSI fg/bg colors.
 */
#ifndef MACJR_HAL_BLIT_H
#define MACJR_HAL_BLIT_H

#include "esp32s3hal/display.h"
#include "tty/vc.h"

#include <stdint.h>

/* Redraw every row flagged in row_dirty (a row_dirty_words-word bitmap, one
 * bit per row) at glyph_w x glyph_h pixels per cell, plus the cursor if
 * visible. Rows not flagged dirty are left untouched on the panel. */
void macjr_hal_blit_vc(macjr_hal_display_t *hal, macjr_vc_t *vc, uint32_t *row_dirty, int glyph_w, int glyph_h,
                       int row_dirty_words);

#endif
