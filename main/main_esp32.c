/*
 * ESP32 demo entry point: brings up the ST7789-backed display HAL and the
 * macjr_tty TTY stack, then draws the portable Turbo C IDE screen (see
 * ide_screen.c) and leaves it on screen. See main_host.c for the macOS/SDL2
 * counterpart -- both share ide_screen.c unmodified.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "tty/tty.h"
#include "display_hal.h"
#include "ide_screen.h"
#include "sdcard.h"

static const char *TAG = "macjr_demo";

void app_main(void) {
  if (sdcard_init() != ESP_OK) {
    ESP_LOGW(TAG, "sdcard_init failed, continuing without SD card");
  }

  macjr_hal_display_t *hal = display_hal_init();
  if (!hal) {
    ESP_LOGE(TAG, "display_hal_init failed");
    return;
  }

  macjr_tty_t *tty = macjr_tty_create(hal, 8, 16); /* matches the built-in 8x16 font */
  if (!tty) {
    ESP_LOGE(TAG, "macjr_tty_create failed");
    return;
  }

  struct macjr_winsize *ws = macjr_tty_winsize(tty);
  ESP_LOGI(TAG, "tty up: %ux%u chars on %ux%u px", ws->ws_col, ws->ws_row, ws->ws_xpixel, ws->ws_ypixel);

  /* Static, one-shot recreation of a classic Turbo C IDE screen. No
   * keyboard input, no redraw loop -- hide the cursor, draw it once, flush
   * once, and leave it on screen. */
  const char *clear = "\x1b[2J\x1b[?25l";
  macjr_tty_write(tty, clear, strlen(clear));
  macjr_draw_turboc_ide(tty);

  int64_t flush_start_us = esp_timer_get_time();
  macjr_display_flush(tty);
  int64_t flush_us = esp_timer_get_time() - flush_start_us;
  ESP_LOGI(TAG, "display flush took %lld.%03lld ms", flush_us / 1000, flush_us % 1000);
}
