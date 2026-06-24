/*
 * macjr_hal_display_t implementation backed by lcd.c's ST7789 SPI driver.
 * Buffers cell/glyph draws into a persistent row window so a full-screen
 * refresh costs a handful of SPI transfers instead of one per cell.
 */
#include "display_hal.h"
#include "lcd.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdint.h>

static const char *TAG = "tty_hal";

static inline uint16_t to565(uint32_t rgb888)
{
    return rgb_to_565((rgb888 >> 16) & 0xFF, (rgb888 >> 8) & 0xFF, rgb888 & 0xFF);
}

/*
 * The console issues a fill_rect + blit_1bpp_mask pair per cell -- a full
 * 30x20 screen is ~1200 calls. Doing one malloc + one blocking SPI transfer
 * per call made a full refresh take well over a second. Instead, accumulate
 * rows of pixels into a single persistent DMA buffer and only hit the SPI
 * bus when a call no longer fits the buffered window (or at end_frame).
 */
#define ROW_BUF_CAP 40 /* px tall, matches lcd.c's BUFFER_LINES sizing */

static uint16_t *s_row_buf;
static int s_row_y0 = -1; /* top y of the buffered window, -1 = empty */
static int s_row_h = 0;   /* valid rows currently held in s_row_buf */

static void flush_row(macjr_hal_display_t *d)
{
    if (s_row_y0 < 0) {
        return;
    }
    lcd_blit_sync(0, s_row_y0, d->width_px, s_row_y0 + s_row_h, s_row_buf);
    s_row_y0 = -1;
    s_row_h = 0;
}

static void hal_end_frame(macjr_hal_display_t *d)
{
    flush_row(d);
}

/* Make sure rows [y, y+h) are in the buffer, flushing/restarting it if not. */
static void ensure_row(macjr_hal_display_t *d, int y, int h)
{
    if (s_row_y0 >= 0 && y >= s_row_y0 && (y + h) <= (s_row_y0 + ROW_BUF_CAP)) {
        int rel = y + h - s_row_y0;
        if (rel > s_row_h) {
            s_row_h = rel;
        }
        return;
    }
    flush_row(d);
    s_row_y0 = y;
    s_row_h = h;
}

/* Fill a solid rectangle. Called per cell (background) and for the cursor. */
static void hal_fill_rect(macjr_hal_display_t *d, int x, int y, int w, int h, uint32_t rgb888)
{
    ensure_row(d, y, h);
    uint16_t c = to565(rgb888);
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= d->height_px) {
            continue;
        }
        uint16_t *line = &s_row_buf[(size_t)(yy - s_row_y0) * (size_t)d->width_px];
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= d->width_px) {
                continue;
            }
            line[xx] = c;
        }
    }
}

/* Expand a 1-bpp glyph mask (MSB = leftmost) into RGB565 in the row buffer. */
static void hal_blit_1bpp(macjr_hal_display_t *d, int x, int y, int w, int h, int row_bytes,
                          const uint8_t *bitmap, uint32_t fg_rgb888, uint32_t bg_rgb888)
{
    ensure_row(d, y, h);
    uint16_t fg = to565(fg_rgb888);
    uint16_t bg = to565(bg_rgb888);
    for (int row = 0; row < h; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= d->height_px) {
            continue;
        }
        const uint8_t *line = bitmap + (size_t)row * row_bytes;
        uint16_t *out = &s_row_buf[(size_t)(yy - s_row_y0) * (size_t)d->width_px];
        for (int col = 0; col < w; col++) {
            int xx = x + col;
            if (xx < 0 || xx >= d->width_px) {
                continue;
            }
            uint8_t bit = (line[col >> 3] >> (7 - (col & 7))) & 1u;
            out[xx] = bit ? fg : bg;
        }
    }
}

/* Bring up the LCD panel and a DMA-capable row buffer, then wire up the
 * macjr_hal_display_t entry points above. See display_hal.h for ownership. */
macjr_hal_display_t *display_hal_init(void)
{
    if (lcd_init() != ESP_OK) {
        ESP_LOGE(TAG, "lcd_init failed");
        return NULL;
    }

    s_row_buf = heap_caps_malloc((size_t)LCD_H_RES * ROW_BUF_CAP * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!s_row_buf) {
        ESP_LOGE(TAG, "failed to allocate row buffer");
        return NULL;
    }

    static macjr_hal_display_t hal;
    hal.ctx = NULL;
    hal.width_px = LCD_H_RES;
    hal.height_px = LCD_V_RES;
    hal.begin_frame = NULL;
    hal.end_frame = hal_end_frame;
    hal.fill_rect = hal_fill_rect;
    hal.blit_1bpp_mask = hal_blit_1bpp;
    return &hal;
}
