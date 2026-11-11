/*
 * Streaming PNG decoder.
 *
 * A PNG file is an 8-byte signature followed by chunks (length, 4-letter
 * type, data, CRC). IHDR gives the size and pixel format, PLTE / tRNS the
 * palette and its transparency, and the IDAT chunks together hold one zlib
 * stream of image data. Inflating that stream (ROM tinfl, into a 32 KB
 * wrap-around window) yields the scanlines: one filter-type byte, then the
 * row's pixels as differences against the row above / pixel to the left.
 * Each row is un-filtered against the previous one, converted to RGB and
 * handed to the caller, so only two rows are ever kept in memory.
 */
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

#define IN_CHUNK 1024 /* compressed bytes read from the file at a time */

static const uint8_t PNG_SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

typedef struct {
  png_header_cb_t on_header;
  png_row_cb_t on_row;
  void *ctx;

  uint32_t width, height;
  uint8_t depth;     /* bits per sample: 1, 2, 4, 8, 16 */
  uint8_t ctype;     /* 0 gray, 2 RGB, 3 palette, 4 gray+alpha, 6 RGBA */
  uint8_t channels;
  size_t bpp;        /* bytes per pixel for the filters, at least 1 */
  size_t stride;     /* bytes per row, without the filter byte */

  uint8_t *cur;      /* filter byte + row being assembled */
  uint8_t *prev;     /* previous row, already un-filtered (same layout) */
  size_t fill;
  uint32_t y;
  uint8_t *rgb;      /* width * 3 output row */

  uint8_t pal[256][3];
  uint8_t alpha[256]; /* palette transparency (tRNS), 255 = opaque */

  tinfl_decompressor *inflator;
  uint8_t *window;   /* TINFL_LZ_DICT_SIZE bytes */
  size_t window_ofs;
  bool stream_done;
} png_t;

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
  int p = a + b - c;
  int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
  if (pa <= pb && pa <= pc) {
    return a;
  }
  return pb <= pc ? b : c;
}

/* Undo the row filter in place: x is this row, up the row above. */
static bool unfilter(png_t *png) {
  uint8_t *x = png->cur + 1;
  const uint8_t *up = png->prev + 1;
  size_t n = png->stride, bpp = png->bpp;
  switch (png->cur[0]) {
  case 0: /* None */
    break;
  case 1: /* Sub */
    for (size_t i = bpp; i < n; i++) x[i] += x[i - bpp];
    break;
  case 2: /* Up */
    for (size_t i = 0; i < n; i++) x[i] += up[i];
    break;
  case 3: /* Average */
    for (size_t i = 0; i < n; i++) x[i] += ((i >= bpp ? x[i - bpp] : 0) + up[i]) >> 1;
    break;
  case 4: /* Paeth */
    for (size_t i = 0; i < n; i++) {
      x[i] += paeth(i >= bpp ? x[i - bpp] : 0, up[i], i >= bpp ? up[i - bpp] : 0);
    }
    break;
  default:
    return false;
  }
  return true;
}

/* Sample k of the row as 0..255 (16-bit samples keep their high byte,
 * 1/2/4-bit ones are scaled up). */
static uint8_t sample(const png_t *png, const uint8_t *row, size_t k) {
  switch (png->depth) {
  case 16:
    return row[k * 2];
  case 8:
    return row[k];
  default: {
    unsigned bit = (unsigned)(k * png->depth);
    unsigned mask = (1u << png->depth) - 1;
    unsigned v = (row[bit / 8] >> (8 - png->depth - bit % 8)) & mask;
    return png->ctype == 3 ? (uint8_t)v : (uint8_t)(v * 255 / mask);
  }
  }
}

static uint8_t blend(uint8_t c, uint8_t a) {
  return (uint8_t)(c * a / 255); /* onto black */
}

static void emit_row(png_t *png) {
  const uint8_t *row = png->cur + 1;
  uint8_t *out = png->rgb;
  for (uint32_t i = 0; i < png->width; i++, out += 3) {
    size_t k = (size_t)i * png->channels;
    uint8_t r, g, b, a = 255;
    switch (png->ctype) {
    case 0:
      r = g = b = sample(png, row, k);
      break;
    case 2:
      r = sample(png, row, k), g = sample(png, row, k + 1), b = sample(png, row, k + 2);
      break;
    case 3: {
      uint8_t idx = sample(png, row, k);
      r = png->pal[idx][0], g = png->pal[idx][1], b = png->pal[idx][2], a = png->alpha[idx];
      break;
    }
    case 4:
      r = g = b = sample(png, row, k), a = sample(png, row, k + 1);
      break;
    default: /* 6 */
      r = sample(png, row, k), g = sample(png, row, k + 1), b = sample(png, row, k + 2);
      a = sample(png, row, k + 3);
      break;
    }
    out[0] = blend(r, a), out[1] = blend(g, a), out[2] = blend(b, a);
  }
  png->on_row(png->ctx, png->y, png->rgb);
}

/* Collect inflated bytes into rows and emit each completed row. */
static bool take_bytes(png_t *png, const uint8_t *p, size_t n) {
  while (n && png->y < png->height) {
    size_t want = png->stride + 1 - png->fill;
    size_t take = n < want ? n : want;
    memcpy(png->cur + png->fill, p, take);
    png->fill += take, p += take, n -= take;
    if (png->fill == png->stride + 1) {
      if (!unfilter(png)) {
        return false;
      }
      emit_row(png);
      uint8_t *t = png->prev;
      png->prev = png->cur, png->cur = t;
      png->fill = 0;
      png->y++;
    }
  }
  return true;
}

/* Feed compressed IDAT bytes through the inflater. */
static bool inflate_bytes(png_t *png, const uint8_t *in, size_t n) {
  while (!png->stream_done) {
    size_t in_size = n;
    size_t out_size = TINFL_LZ_DICT_SIZE - png->window_ofs;
    tinfl_status st = tinfl_decompress(png->inflator, in, &in_size, png->window, png->window + png->window_ofs,
                                       &out_size, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
    in += in_size, n -= in_size;
    if (!take_bytes(png, png->window + png->window_ofs, out_size)) {
      return false;
    }
    png->window_ofs = (png->window_ofs + out_size) & (TINFL_LZ_DICT_SIZE - 1);
    if (st < 0) {
      return false;
    }
    if (st == TINFL_STATUS_DONE) {
      png->stream_done = true;
    } else if (st == TINFL_STATUS_NEEDS_MORE_INPUT && n == 0) {
      break; /* wait for the next IDAT bytes */
    }
  }
  return true;
}

static esp_err_t read_header(png_t *png, const uint8_t ihdr[13]) {
  png->width = be32(ihdr);
  png->height = be32(ihdr + 4);
  png->depth = ihdr[8];
  png->ctype = ihdr[9];
  if (ihdr[12] != 0) {
    return ESP_ERR_NOT_SUPPORTED; /* interlaced */
  }
  if (png->width == 0 || png->height == 0 || png->width > PNG_MAX_WIDTH) {
    return ESP_ERR_NOT_SUPPORTED;
  }
  bool ok;
  switch (png->ctype) {
  case 0: png->channels = 1; ok = png->depth <= 16 && (png->depth & (png->depth - 1)) == 0; break;
  case 2: png->channels = 3; ok = png->depth == 8 || png->depth == 16; break;
  case 3: png->channels = 1; ok = png->depth <= 8 && (png->depth & (png->depth - 1)) == 0; break;
  case 4: png->channels = 2; ok = png->depth == 8 || png->depth == 16; break;
  case 6: png->channels = 4; ok = png->depth == 8 || png->depth == 16; break;
  default: ok = false; break;
  }
  if (!ok) {
    return ESP_ERR_INVALID_RESPONSE;
  }
  size_t bits = (size_t)png->channels * png->depth;
  png->stride = (png->width * bits + 7) / 8;
  png->bpp = bits >= 8 ? bits / 8 : 1;
  png->cur = calloc(1, png->stride + 1);
  png->prev = calloc(1, png->stride + 1); /* row "above" the first is zero */
  png->rgb = malloc((size_t)png->width * 3);
  png->inflator = malloc(sizeof(tinfl_decompressor));
  png->window = malloc(TINFL_LZ_DICT_SIZE);
  if (!png->cur || !png->prev || !png->rgb || !png->inflator || !png->window) {
    return ESP_ERR_NO_MEM;
  }
  tinfl_init(png->inflator);
  for (int i = 0; i < 256; i++) {
    png->alpha[i] = 255;
  }
  return ESP_OK;
}

static esp_err_t decode(png_t *png, FILE *f) {
  uint8_t buf[IN_CHUNK];
  bool have_header = false;

  if (fread(buf, 1, 8, f) != 8 || memcmp(buf, PNG_SIG, 8) != 0) {
    return ESP_ERR_INVALID_RESPONSE;
  }
  for (;;) {
    if (fread(buf, 1, 8, f) != 8) {
      return ESP_ERR_INVALID_RESPONSE; /* ended without IEND */
    }
    uint32_t len = be32(buf);
    char type[5];
    memcpy(type, buf + 4, 4);
    type[4] = '\0';

    if (!have_header && strcmp(type, "IHDR") != 0) {
      return ESP_ERR_INVALID_RESPONSE;
    }
    if (strcmp(type, "IEND") == 0) {
      break;
    }
    if (strcmp(type, "IHDR") == 0) {
      if (len != 13 || fread(buf, 1, 13, f) != 13) {
        return ESP_ERR_INVALID_RESPONSE;
      }
      esp_err_t ret = read_header(png, buf);
      if (ret != ESP_OK) {
        return ret;
      }
      have_header = true;
      if (!png->on_header(png->ctx, png->width, png->height)) {
        return ESP_OK;
      }
    } else if (strcmp(type, "PLTE") == 0 && len <= sizeof(png->pal)) {
      if (fread(png->pal, 1, len, f) != len) {
        return ESP_ERR_INVALID_RESPONSE;
      }
    } else if (strcmp(type, "tRNS") == 0 && png->ctype == 3 && len <= sizeof(png->alpha)) {
      if (fread(png->alpha, 1, len, f) != len) {
        return ESP_ERR_INVALID_RESPONSE;
      }
    } else if (strcmp(type, "IDAT") == 0) {
      for (uint32_t left = len; left > 0;) {
        size_t n = left < IN_CHUNK ? left : IN_CHUNK;
        if (fread(buf, 1, n, f) != n || !inflate_bytes(png, buf, n)) {
          return ESP_ERR_INVALID_RESPONSE;
        }
        left -= n;
      }
    } else if (fseek(f, len, SEEK_CUR) != 0) { /* chunk we don't need */
      return ESP_ERR_INVALID_RESPONSE;
    }
    if (fseek(f, 4, SEEK_CUR) != 0) { /* CRC (not checked; zlib has its own) */
      return ESP_ERR_INVALID_RESPONSE;
    }
  }
  return png->y == png->height ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t png_decode_file(const char *path, png_header_cb_t on_header, png_row_cb_t on_row, void *ctx) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return ESP_ERR_NOT_FOUND;
  }
  png_t *png = calloc(1, sizeof(png_t));
  if (!png) {
    fclose(f);
    return ESP_ERR_NO_MEM;
  }
  png->on_header = on_header;
  png->on_row = on_row;
  png->ctx = ctx;

  esp_err_t ret = decode(png, f);

  fclose(f);
  free(png->cur);
  free(png->prev);
  free(png->rgb);
  free(png->inflator);
  free(png->window);
  free(png);
  return ret;
}
