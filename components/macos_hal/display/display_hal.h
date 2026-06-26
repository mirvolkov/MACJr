/*
 * macjr_hal_display_t backend for the host (macOS/SDL2) build -- the
 * desktop counterpart to esp32_st7789's real-panel backend. Lets the same
 * tty/vc/font stack run in a window for development without hardware.
 */
#ifndef MACJR_MACOS_DISPLAY_HAL_H
#define MACJR_MACOS_DISPLAY_HAL_H

#include "tty/hal_display.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open an SDL window of width_px x height_px and return a macjr_hal_display_t
 * that renders into it. Returns NULL on failure (and logs to stderr). The
 * returned pointer is owned by this module (static storage) -- do not free
 * it; call macos_display_hal_shutdown() instead. */
macjr_hal_display_t *macos_display_hal_init(int width_px, int height_px, const char *title);

/* Pump pending SDL events (window close, etc). Call this periodically from
 * an idle loop after the last macjr_display_flush() so the window stays
 * responsive even once there's nothing left to draw. */
void macos_display_hal_pump(macjr_hal_display_t *hal);

/* True once the user has closed the window (SDL_QUIT) -- callers should
 * exit their main loop. */
bool macos_display_hal_should_close(macjr_hal_display_t *hal);

/* Tear down the SDL window/renderer/texture. */
void macos_display_hal_shutdown(macjr_hal_display_t *hal);

#ifdef __cplusplus
}
#endif

#endif
