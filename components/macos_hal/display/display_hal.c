/*
 * macjr_hal_display_t implementation backed by SDL2: a single RGB888
 * framebuffer that fill_rect/blit_1bpp_mask write into directly, pushed to
 * a streaming texture and presented once per frame (end_frame). No row
 * buffering tricks needed here -- unlike SPI to a real panel, blitting a
 * 320x240 framebuffer to a desktop window is essentially free.
 */
#include "display_hal.h"

#include <SDL2/SDL.h>

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  uint32_t *fb; /* width_px * height_px, one rgb888 value (0x00RRGGBB) per pixel */
  int width_px;
  int height_px;
  bool should_close;
} macos_hal_ctx_t;

static inline void put_px(macos_hal_ctx_t *ctx, int x, int y, uint32_t rgb888) {
  if (x < 0 || x >= ctx->width_px || y < 0 || y >= ctx->height_px) {
    return;
  }
  ctx->fb[(size_t)y * (size_t)ctx->width_px + (size_t)x] = rgb888;
}

static void hal_fill_rect(macjr_hal_display_t *d, int x, int y, int w, int h, uint32_t rgb888) {
  macos_hal_ctx_t *ctx = (macos_hal_ctx_t *)d->ctx;
  for (int yy = y; yy < y + h; yy++) {
    for (int xx = x; xx < x + w; xx++) {
      put_px(ctx, xx, yy, rgb888);
    }
  }
}

static void hal_blit_1bpp(macjr_hal_display_t *d, int x, int y, int w, int h, int row_bytes,
                          const uint8_t *bitmap, uint32_t fg_rgb888, uint32_t bg_rgb888) {
  macos_hal_ctx_t *ctx = (macos_hal_ctx_t *)d->ctx;
  for (int row = 0; row < h; row++) {
    const uint8_t *line = bitmap + (size_t)row * (size_t)row_bytes;
    for (int col = 0; col < w; col++) {
      uint8_t bit = (line[col >> 3] >> (7 - (col & 7))) & 1u;
      put_px(ctx, x + col, y + row, bit ? fg_rgb888 : bg_rgb888);
    }
  }
}

static void pump_events(macos_hal_ctx_t *ctx) {
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    if (ev.type == SDL_QUIT) {
      ctx->should_close = true;
    }
  }
}

static void hal_end_frame(macjr_hal_display_t *d) {
  macos_hal_ctx_t *ctx = (macos_hal_ctx_t *)d->ctx;
  SDL_UpdateTexture(ctx->texture, NULL, ctx->fb, ctx->width_px * (int)sizeof(uint32_t));
  SDL_RenderClear(ctx->renderer);
  SDL_RenderCopy(ctx->renderer, ctx->texture, NULL, NULL);
  SDL_RenderPresent(ctx->renderer);
  pump_events(ctx);
}

macjr_hal_display_t *macos_display_hal_init(int width_px, int height_px, const char *title) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return NULL;
  }

  macos_hal_ctx_t *ctx = (macos_hal_ctx_t *)calloc(1, sizeof(*ctx));
  if (!ctx) {
    return NULL;
  }
  ctx->width_px = width_px;
  ctx->height_px = height_px;
  ctx->fb = (uint32_t *)calloc((size_t)width_px * (size_t)height_px, sizeof(uint32_t));
  if (!ctx->fb) {
    free(ctx);
    return NULL;
  }

  /* Scale the window up so a 320x240 panel isn't a postage stamp on a
   * modern display; the texture itself stays panel-resolution. */
  int window_scale = 2;
  ctx->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  width_px * window_scale, height_px * window_scale, SDL_WINDOW_SHOWN);
  if (!ctx->window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    free(ctx->fb);
    free(ctx);
    return NULL;
  }

  ctx->renderer = SDL_CreateRenderer(ctx->window, -1, SDL_RENDERER_ACCELERATED);
  if (!ctx->renderer) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    SDL_DestroyWindow(ctx->window);
    free(ctx->fb);
    free(ctx);
    return NULL;
  }
  SDL_RenderSetLogicalSize(ctx->renderer, width_px, height_px);

  /* RGB888 here is SDL's packed-32 "XRGB8888 in host order" format, which
   * matches our 0x00RRGGBB uint32_t values directly -- no per-pixel
   * conversion needed between the HAL's rgb888 and the texture. */
  ctx->texture = SDL_CreateTexture(ctx->renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width_px,
                                    height_px);
  if (!ctx->texture) {
    fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    SDL_DestroyRenderer(ctx->renderer);
    SDL_DestroyWindow(ctx->window);
    free(ctx->fb);
    free(ctx);
    return NULL;
  }

  static macjr_hal_display_t hal;
  hal.ctx = ctx;
  hal.width_px = width_px;
  hal.height_px = height_px;
  hal.begin_frame = NULL;
  hal.end_frame = hal_end_frame;
  hal.fill_rect = hal_fill_rect;
  hal.blit_1bpp_mask = hal_blit_1bpp;
  return &hal;
}

void macos_display_hal_pump(macjr_hal_display_t *hal) {
  pump_events((macos_hal_ctx_t *)hal->ctx);
}

bool macos_display_hal_should_close(macjr_hal_display_t *hal) {
  return ((macos_hal_ctx_t *)hal->ctx)->should_close;
}

void macos_display_hal_shutdown(macjr_hal_display_t *hal) {
  macos_hal_ctx_t *ctx = (macos_hal_ctx_t *)hal->ctx;
  SDL_DestroyTexture(ctx->texture);
  SDL_DestroyRenderer(ctx->renderer);
  SDL_DestroyWindow(ctx->window);
  SDL_Quit();
  free(ctx->fb);
  free(ctx);
  hal->ctx = NULL;
}
