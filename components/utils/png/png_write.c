/*
 * PNG writer: 8-bit RGB, no filtering, the image data in stored (not
 * compressed) deflate blocks, one row per block and IDAT chunk. No
 * compressor and no buffer beyond the caller's image; the file is about
 * width * height * 3 bytes.
 */
#include "png.h"

#include <stdio.h>
#include <string.h>

#define ADLER_MOD 65521

static const uint8_t SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

/* CRC-32 of the PNG spec, continued from `crc` (start with 0). */
static uint32_t crc32_add(uint32_t crc, const uint8_t *data, size_t len) {
  crc = ~crc;
  while (len--) {
    crc ^= *data++;
    for (int i = 0; i < 8; i++) {
      crc = crc >> 1 ^ (crc & 1 ? 0xEDB88320u : 0);
    }
  }
  return ~crc;
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* A chunk being written: its data comes in pieces, the CRC runs along. */
typedef struct {
  FILE *f;
  uint32_t crc;
  bool ok;
} chunk_t;

static void chunk_add(chunk_t *c, const void *data, size_t len) {
  c->crc = crc32_add(c->crc, data, len);
  c->ok = c->ok && fwrite(data, 1, len, c->f) == len;
}

static void chunk_begin(chunk_t *c, const char *type, uint32_t len) {
  uint8_t head[4];
  put32(head, len);
  c->ok = c->ok && fwrite(head, 1, sizeof(head), c->f) == sizeof(head);
  c->crc = 0;
  chunk_add(c, type, 4);
}

static void chunk_end(chunk_t *c) {
  uint8_t tail[4];
  put32(tail, c->crc);
  c->ok = c->ok && fwrite(tail, 1, sizeof(tail), c->f) == sizeof(tail);
}

esp_err_t png_write_file(const char *path, uint32_t width, uint32_t height, const uint8_t *rgb) {
  size_t row_bytes = (size_t)width * 3;
  size_t block = 1 + row_bytes; /* filter byte + pixels */
  if (width == 0 || height == 0 || block > 0xFFFF) {
    return ESP_ERR_INVALID_ARG;
  }
  FILE *f = fopen(path, "wb");
  if (!f) {
    return ESP_ERR_NOT_FOUND;
  }
  chunk_t c = {.f = f, .ok = fwrite(SIGNATURE, 1, sizeof(SIGNATURE), f) == sizeof(SIGNATURE)};

  uint8_t ihdr[13] = {[8] = 8, [9] = 2}; /* 8 bits, RGB; defaults for the rest */
  put32(ihdr, width);
  put32(ihdr + 4, height);
  chunk_begin(&c, "IHDR", sizeof(ihdr));
  chunk_add(&c, ihdr, sizeof(ihdr));
  chunk_end(&c);

  const uint8_t zlib_header[2] = {0x78, 0x01};
  uint32_t adler_a = 1, adler_b = 0;
  for (uint32_t y = 0; y < height && c.ok; y++) {
    const uint8_t *row = rgb + y * row_bytes;
    uint8_t filter = 0;
    uint8_t stored[5] = {y == height - 1, (uint8_t)block, (uint8_t)(block >> 8), (uint8_t)~block,
                         (uint8_t)(~block >> 8)};
    chunk_begin(&c, "IDAT", (y == 0 ? sizeof(zlib_header) : 0) + sizeof(stored) + block);
    if (y == 0) {
      chunk_add(&c, zlib_header, sizeof(zlib_header));
    }
    chunk_add(&c, stored, sizeof(stored));
    chunk_add(&c, &filter, 1);
    chunk_add(&c, row, row_bytes);
    chunk_end(&c);

    adler_b = (adler_b + adler_a) % ADLER_MOD; /* the filter byte, 0 */
    for (size_t i = 0; i < row_bytes; i++) {
      adler_a = (adler_a + row[i]) % ADLER_MOD;
      adler_b = (adler_b + adler_a) % ADLER_MOD;
    }
  }

  uint8_t adler[4];
  put32(adler, adler_b << 16 | adler_a);
  chunk_begin(&c, "IDAT", sizeof(adler));
  chunk_add(&c, adler, sizeof(adler));
  chunk_end(&c);
  chunk_begin(&c, "IEND", 0);
  chunk_end(&c);

  bool ok = fclose(f) == 0 && c.ok;
  if (!ok) {
    remove(path);
  }
  return ok ? ESP_OK : ESP_FAIL;
}
