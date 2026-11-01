#ifndef MACJR_LCD_H
#define MACJR_LCD_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Panel geometry (landscape: 320 wide x 240 tall). */
#define LCD_H_RES 320
#define LCD_V_RES 240

extern esp_lcd_panel_handle_t panel_handle;

/* Bring up SPI3 + ST7789. Safe to call once. */
esp_err_t lcd_init(void);

/* RGB888 -> RGB565, byte-swapped so the panel (which reads the high byte
 * first) gets it right when the buffer is sent as-is over SPI. */
uint16_t rgb_to_565(uint8_t r, uint8_t g, uint8_t b);

/*
 * Start drawing an RGB565 buffer into [x1,x2) x [y1,y2) and return without
 * waiting for the DMA transfer. `buf` must stay untouched until
 * lcd_wait(ticket) returns. Coordinates are exclusive on the high end, like
 * esp_lcd_panel_draw_bitmap.
 */
uint32_t lcd_blit_async(int x1, int y1, int x2, int y2, const void *buf);

/* Block until the transfer lcd_blit_async() returned `ticket` for (and every
 * transfer before it) has finished. */
void lcd_wait(uint32_t ticket);

#ifdef __cplusplus
}
#endif

#endif
