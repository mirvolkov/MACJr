/*
 * Firmware entry point: brings up the board HALs (SD card, keyboard,
 * USB UAC mic, ST7789 display) and the macjr_tty TTY stack, then draws the Turbo C IDE
 * demo screen (see ide_screen.c) and leaves it on screen.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "tty/tty.h"
#include "display_hal.h"
#include "ide_screen.h"
#include "sdcard.h"
#include "matmul_bench.h"
#include "keyboard.h"
#include "mic.h"

static const char *TAG = "macjr_demo";

void app_main(void) {
  // matmul_bench_run();

  if (sdcard_init() != ESP_OK) {
    ESP_LOGW(TAG, "sdcard_init failed, continuing without SD card");
  }

  if (keyboard_init() != ESP_OK || keyboard_matrix_test_start() != ESP_OK) {
    ESP_LOGW(TAG, "keyboard init failed, continuing without keyboard");
  }

  // if (mic_init() != ESP_OK || mic_uac_start() != ESP_OK) {
  //   ESP_LOGW(TAG, "mic/UAC init failed, continuing without USB mic");
  // }

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
