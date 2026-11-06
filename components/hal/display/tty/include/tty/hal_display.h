/*
 * Hardware abstraction: your TFT driver implements this.
 * The TTY/console stack only calls these entry points (Linux "fbdev" / DRM KMS analog).
 */
#ifndef MACJR_HAL_DISPLAY_H
#define MACJR_HAL_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

typedef struct macjr_hal_display macjr_hal_display_t;

struct macjr_hal_display {
  void *ctx;
  int width_px;
  int height_px;

  /* Optional: tear control / lock panel bus between begin/end. */
  void (*begin_frame)(macjr_hal_display_t *d);
  void (*end_frame)(macjr_hal_display_t *d);

  void (*fill_rect)(macjr_hal_display_t *d, int x, int y, int w, int h, uint32_t rgb888);

  /*
   * Draw an w x h 1-bpp mask; each row is row_bytes bytes, MSB is leftmost pixel.
   * Pixels with bit=1 get fg, else bg (for opaque text background).
   */
  void (*blit_1bpp_mask)(macjr_hal_display_t *d, int x, int y, int w, int h, int row_bytes,
                         const uint8_t *bitmap, uint32_t fg_rgb888, uint32_t bg_rgb888);

  /* Optional: draw a w x h true-color image, 3 bytes per pixel (R, G, B),
   * rows packed (w * 3 bytes each). NULL if the panel can't do it. */
  void (*blit_rgb888)(macjr_hal_display_t *d, int x, int y, int w, int h, const uint8_t *rgb);

  /* Optional: copy what the panel shows into `rgb`, width_px * height_px
   * pixels of 3 bytes (R, G, B). NULL if the panel can't do it. */
  void (*read_rgb888)(macjr_hal_display_t *d, uint8_t *rgb);
};

#endif
