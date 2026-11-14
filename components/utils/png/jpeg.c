/*
 * Baseline JPEG decoder, presenting the same streaming header/row callbacks as
 * png.c so the image viewer can share one drawing path. It wraps the TJpgDec
 * decompressor that lives in the ESP32-S3 ROM (jd_prepare / jd_decomp, built
 * with JD_FORMAT 0 = RGB888), so it costs no extra code or dependency, the way
 * png.c uses the ROM's zlib inflater.
 *
 * TJpgDec hands back pixels in MCU rectangles, left to right then top to
 * bottom. We gather one MCU-row-tall band (full width) and, once the band is
 * complete, emit its scanlines through on_row -- so the caller still sees rows
 * top to bottom. Oversized images are down-scaled by a power of two during
 * decode (TJpgDec's own feature) so the result lands near the 320x240 screen.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "png.h" /* png_header_cb_t / png_row_cb_t */
#include "rom/tjpgd.h"

/* The board's panel. Decoding below this wastes nothing the viewer can show;
 * the viewer does the final (small) nearest-neighbor fit. */
#define SCREEN_W 320
#define SCREEN_H 240

#define FILE_BUF 16384  /* one large SD read per refill, as in png.c */
#define POOL_SIZE 8192  /* TJpgDec work pool: tables + IDCT + MCU buffers */
#define MCU_MAX_H 16    /* tallest MCU output row (2 blocks, no descale) */

typedef struct {
  FILE *f;
  png_row_cb_t on_row;
  void *ctx;
  uint32_t out_w, out_h; /* reported (possibly down-scaled) size */
  uint8_t *band;         /* out_w * MCU_MAX_H * 3 */
  int band_top;          /* output y of the band's top row, -1 = none */
  int band_h;
} jctx_t;

/* TJpgDec input: fill `buff` with up to `n` bytes, or skip `n` when buff==NULL. */
static UINT in_func(JDEC *jd, BYTE *buff, UINT n) {
  jctx_t *j = jd->device;
  if (buff) {
    return (UINT)fread(buff, 1, n, j->f);
  }
  return fseek(j->f, (long)n, SEEK_CUR) == 0 ? n : 0;
}

static void emit_band(jctx_t *j) {
  if (j->band_top < 0) {
    return;
  }
  for (int r = 0; r < j->band_h; r++) {
    uint32_t y = (uint32_t)(j->band_top + r);
    if (y >= j->out_h) {
      break;
    }
    j->on_row(j->ctx, y, j->band + (size_t)r * j->out_w * 3);
  }
  j->band_top = -1;
}

/* TJpgDec output: one MCU block of RGB888 at `rect` (coords inclusive). */
static UINT out_func(JDEC *jd, void *bitmap, JRECT *rect) {
  jctx_t *j = jd->device;
  int top = rect->top, left = rect->left;
  int bw = rect->right - rect->left + 1;
  int rh = rect->bottom - rect->top + 1;

  if (top != j->band_top) {
    emit_band(j); /* the previous MCU-row is complete */
    j->band_top = top;
    j->band_h = rh;
  }
  if (left < (int)j->out_w) {
    int w = left + bw > (int)j->out_w ? (int)j->out_w - left : bw;
    const uint8_t *src = bitmap;
    for (int r = 0; r < rh && r < MCU_MAX_H; r++) {
      memcpy(j->band + ((size_t)r * j->out_w + left) * 3, src + (size_t)r * bw * 3, (size_t)w * 3);
    }
  }
  return 1; /* keep going */
}

esp_err_t jpeg_decode_file(const char *path, png_header_cb_t on_header, png_row_cb_t on_row, void *ctx) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return ESP_ERR_NOT_FOUND;
  }
  setvbuf(f, NULL, _IOFBF, FILE_BUF);

  void *pool = malloc(POOL_SIZE);
  jctx_t *j = calloc(1, sizeof(jctx_t));
  if (!pool || !j) {
    free(pool);
    free(j);
    fclose(f);
    return ESP_ERR_NO_MEM;
  }
  j->f = f;
  j->on_row = on_row;
  j->ctx = ctx;
  j->band_top = -1;

  esp_err_t ret;
  JDEC jd;
  JRESULT r = jd_prepare(&jd, in_func, pool, POOL_SIZE, j);
  if (r != JDR_OK) {
    ret = r == JDR_FMT3 ? ESP_ERR_NOT_SUPPORTED : (r == JDR_MEM1 || r == JDR_MEM2) ? ESP_ERR_NO_MEM
                                                                                   : ESP_ERR_INVALID_RESPONSE;
    goto done;
  }

  /* Down-scale by a power of two while the result still covers the screen. */
  BYTE scale = 0;
  while (scale < 3 && (jd.width >> (scale + 1)) >= SCREEN_W && (jd.height >> (scale + 1)) >= SCREEN_H) {
    scale++;
  }
  j->out_w = jd.width >> scale;
  j->out_h = jd.height >> scale;
  if (j->out_w == 0 || j->out_h == 0) {
    ret = ESP_ERR_INVALID_RESPONSE;
    goto done;
  }

  if (!on_header(ctx, j->out_w, j->out_h)) {
    ret = ESP_OK; /* caller declined (e.g. out of memory for its own buffer) */
    goto done;
  }

  j->band = malloc((size_t)j->out_w * MCU_MAX_H * 3);
  if (!j->band) {
    ret = ESP_ERR_NO_MEM;
    goto done;
  }

  r = jd_decomp(&jd, out_func, scale);
  emit_band(j); /* flush the last band */
  ret = r == JDR_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE;

done:
  free(j->band);
  free(j);
  free(pool);
  fclose(f);
  return ret;
}
