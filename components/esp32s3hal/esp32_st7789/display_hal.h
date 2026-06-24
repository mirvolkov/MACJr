#ifndef MACJR_DISPLAY_HAL_H
#define MACJR_DISPLAY_HAL_H

#include "esp32s3hal/display.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the ST7789 panel and return a macjr_hal_display_t that renders the
 * console onto it. Returns NULL on failure. The returned pointer is owned by
 * this module (static storage) — do not free it.
 */
macjr_hal_display_t *display_hal_init(void);

#ifdef __cplusplus
}
#endif

#endif
