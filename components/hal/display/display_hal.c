/*
 * macjr_hal_display_t implementation backed by lcd.c's ST7789 SPI driver.
 * Buffers cell/glyph draws into a persistent row window so a full-screen
 * refresh costs a handful of SPI transfers instead of one per cell, and
 * keeps two such windows so the next one is drawn while DMA sends the last.
 */
#include "display_hal.h"
#include "lcd.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdbool.h>
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
 *
 * The window is double-buffered: flush_row() only queues the transfer and
 * switches to the other buffer, so drawing the next window overlaps with
 * sending this one. A buffer is reused only after its last transfer is done.
 */
#define ROW_BUF_CAP 40 /* px tall, matches lcd.c's BUFFER_LINES sizing */

static uint16_t *s_bufs[2];      /* both the same buffer if the second didn't fit */
static uint32_t s_buf_ticket[2]; /* lcd_wait() ticket of each buffer's last transfer */
static int s_cur;                /* buffer being drawn into */
static uint16_t *s_row_buf;      /* s_bufs[s_cur] */
static int s_row_y0 = -1; /* top y of the buffered window, -1 = empty */
static int s_row_h = 0;   /* valid rows currently held in s_row_buf */

/*
 * Pixels written in the current window, per buffered row: [x0, x1), empty
 * when x0 >= x1. The buffer is reused between windows without clearing, so
 * only these spans hold valid pixels; anything else is stale data from an
 * earlier window and must never reach the panel. ensure_row() keeps each
 * span gap-free.
 */
static int16_t s_dirty_x0[ROW_BUF_CAP];
static int16_t s_dirty_x1[ROW_BUF_CAP];

/* Send the dirty spans: runs of fully written rows go out as one transfer
 * (the common console case), partial rows one segment at a time. */
static void flush_row(macjr_hal_display_t *d)
{
    if (s_row_y0 < 0) {
        return;
    }
    int run = -1; /* first row of the pending full-width run */
    uint32_t ticket = 0;
    bool sent = false;
    for (int r = 0; r <= s_row_h; r++) {
        bool full = r < s_row_h && s_dirty_x0[r] == 0 && s_dirty_x1[r] == d->width_px;
        if (full) {
            if (run < 0) {
                run = r;
            }
            continue;
        }
        if (run >= 0) {
            ticket = lcd_blit_async(0, s_row_y0 + run, d->width_px, s_row_y0 + r,
                                    &s_row_buf[(size_t)run * (size_t)d->width_px]);
            sent = true;
            run = -1;
        }
        if (r < s_row_h && s_dirty_x0[r] < s_dirty_x1[r]) {
            ticket = lcd_blit_async(s_dirty_x0[r], s_row_y0 + r, s_dirty_x1[r], s_row_y0 + r + 1,
                                    &s_row_buf[(size_t)r * (size_t)d->width_px + s_dirty_x0[r]]);
            sent = true;
        }
    }
    if (sent) {
        /* Leave this buffer to DMA and draw on in the other one, once the
         * transfer that last read it is done. */
        s_buf_ticket[s_cur] = ticket;
        s_cur ^= 1;
        s_row_buf = s_bufs[s_cur];
        lcd_wait(s_bufs[0] == s_bufs[1] ? ticket : s_buf_ticket[s_cur]);
    }
    s_row_y0 = -1;
    s_row_h = 0;
}

static void hal_end_frame(macjr_hal_display_t *d)
{
    flush_row(d);
}

/* Would writing [x0, x1) on buffered row r leave unwritten pixels between it
 * and that row's existing span? */
static bool leaves_gap(int r, int x0, int x1)
{
    return s_dirty_x0[r] < s_dirty_x1[r] && (x0 > s_dirty_x1[r] || x1 < s_dirty_x0[r]);
}

/*
 * Make sure the on-screen part of rect (x, y, w, h) is in the buffer and
 * mark it written, flushing/restarting the window if the rect doesn't fit
 * or would split a row's span. Returns false if nothing is on screen.
 */
static bool ensure_row(macjr_hal_display_t *d, int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x;
    int x1 = x + w > d->width_px ? d->width_px : x + w;
    int y0 = y < 0 ? 0 : y;
    int y1 = y + h > d->height_px ? d->height_px : y + h;
    if (x0 >= x1 || y0 >= y1) {
        return false;
    }

    bool fits = s_row_y0 >= 0 && y0 >= s_row_y0 && y1 <= s_row_y0 + ROW_BUF_CAP;
    for (int yy = y0; fits && yy < y1; yy++) {
        int r = yy - s_row_y0;
        fits = r >= s_row_h || !leaves_gap(r, x0, x1);
    }
    if (!fits) {
        flush_row(d);
        s_row_y0 = y0;
    }

    int rel = y1 - s_row_y0;
    for (int r = s_row_h; r < rel; r++) {
        s_dirty_x0[r] = 0;
        s_dirty_x1[r] = 0;
    }
    if (rel > s_row_h) {
        s_row_h = rel;
    }
    for (int r = y0 - s_row_y0; r < rel; r++) {
        if (s_dirty_x0[r] >= s_dirty_x1[r]) {
            s_dirty_x0[r] = x0;
            s_dirty_x1[r] = x1;
        } else {
            if (x0 < s_dirty_x0[r]) s_dirty_x0[r] = x0;
            if (x1 > s_dirty_x1[r]) s_dirty_x1[r] = x1;
        }
    }
    return true;
}

/* Fill a solid rectangle. Called per cell (background) and for the cursor. */
static void fill_band(macjr_hal_display_t *d, int x, int y, int w, int h, uint32_t rgb888)
{
    if (!ensure_row(d, x, y, w, h)) {
        return;
    }
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
static void blit_band(macjr_hal_display_t *d, int x, int y, int w, int h, int row_bytes,
                      const uint8_t *bitmap, uint32_t fg_rgb888, uint32_t bg_rgb888)
{
    if (!ensure_row(d, x, y, w, h)) {
        return;
    }
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

/* Convert packed RGB888 pixels into RGB565 in the row buffer. */
static void rgb_band(macjr_hal_display_t *d, int x, int y, int w, int h, const uint8_t *rgb)
{
    if (!ensure_row(d, x, y, w, h)) {
        return;
    }
    for (int row = 0; row < h; row++) {
        int yy = y + row;
        if (yy < 0 || yy >= d->height_px) {
            continue;
        }
        const uint8_t *src = rgb + (size_t)row * w * 3;
        uint16_t *out = &s_row_buf[(size_t)(yy - s_row_y0) * (size_t)d->width_px];
        for (int col = 0; col < w; col++, src += 3) {
            int xx = x + col;
            if (xx < 0 || xx >= d->width_px) {
                continue;
            }
            out[xx] = rgb_to_565(src[0], src[1], src[2]);
        }
    }
}

/* The row buffer holds ROW_BUF_CAP rows, so taller draws go in bands. */
static void hal_fill_rect(macjr_hal_display_t *d, int x, int y, int w, int h, uint32_t rgb888)
{
    for (int done = 0; done < h; done += ROW_BUF_CAP) {
        int bh = h - done < ROW_BUF_CAP ? h - done : ROW_BUF_CAP;
        fill_band(d, x, y + done, w, bh, rgb888);
    }
}

static void hal_blit_1bpp(macjr_hal_display_t *d, int x, int y, int w, int h, int row_bytes,
                          const uint8_t *bitmap, uint32_t fg_rgb888, uint32_t bg_rgb888)
{
    for (int done = 0; done < h; done += ROW_BUF_CAP) {
        int bh = h - done < ROW_BUF_CAP ? h - done : ROW_BUF_CAP;
        blit_band(d, x, y + done, w, bh, row_bytes, bitmap + (size_t)done * row_bytes, fg_rgb888,
                  bg_rgb888);
    }
}

static void hal_blit_rgb888(macjr_hal_display_t *d, int x, int y, int w, int h, const uint8_t *rgb)
{
    for (int done = 0; done < h; done += ROW_BUF_CAP) {
        int bh = h - done < ROW_BUF_CAP ? h - done : ROW_BUF_CAP;
        rgb_band(d, x, y + done, w, bh, rgb + (size_t)done * w * 3);
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

    size_t buf_size = (size_t)LCD_H_RES * ROW_BUF_CAP * sizeof(uint16_t);
    s_bufs[0] = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    if (!s_bufs[0]) {
        ESP_LOGE(TAG, "failed to allocate row buffer");
        return NULL;
    }
    s_bufs[1] = heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
    if (!s_bufs[1]) {
        ESP_LOGW(TAG, "no room for a second row buffer, drawing and sending in turn");
        s_bufs[1] = s_bufs[0];
    }
    s_row_buf = s_bufs[0];

    static macjr_hal_display_t hal;
    hal.ctx = NULL;
    hal.width_px = LCD_H_RES;
    hal.height_px = LCD_V_RES;
    hal.begin_frame = NULL;
    hal.end_frame = hal_end_frame;
    hal.fill_rect = hal_fill_rect;
    hal.blit_1bpp_mask = hal_blit_1bpp;
    hal.blit_rgb888 = hal_blit_rgb888;
    return &hal;
}
