#ifndef MACJR_PNG_H
#define MACJR_PNG_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Largest width decoded; rows are buffered, the whole image never is. */
#define PNG_MAX_WIDTH 2048

/* Called once, after the header: image size. Return false to stop. */
typedef bool (*png_header_cb_t)(void *ctx, uint32_t width, uint32_t height);

/* Called for every row, top to bottom: `rgb` is width * 3 bytes (R, G, B).
 * Transparent pixels are blended onto black. */
typedef void (*png_row_cb_t)(void *ctx, uint32_t y, const uint8_t *rgb);

/* Decode the PNG file at `path`. Handles all standard color types and bit
 * depths, palettes and palette transparency; not interlaced (Adam7) images.
 * Returns ESP_ERR_NOT_FOUND if the file can't be opened,
 * ESP_ERR_NOT_SUPPORTED for interlaced or too-wide images, ESP_ERR_INVALID_RESPONSE
 * for a damaged file. */
esp_err_t png_decode_file(const char *path, png_header_cb_t on_header, png_row_cb_t on_row, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
