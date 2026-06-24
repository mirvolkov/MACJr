/*
 * MACJr demo entry point: brings up the ST7789-backed display HAL and the
 * macjr_tty TTY stack, then exercises writes, ANSI colors, simulated
 * keyboard input, and the pseudographics font page before settling into a
 * live uptime-counter redraw loop.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "tty/tty.h"
#include "display_hal.h"

static const char *TAG = "macjr_demo";

/* Render the symbolic console grid to the log so the result is observable. */
static void dump_vc(macjr_tty_t *tty) {
  macjr_vc_t *vc = macjr_tty_vc(tty);
  char line[160];
  ESP_LOGI(TAG, "console %dx%d, cursor (%d,%d):", vc->cols, vc->rows, vc->cursor_r, vc->cursor_c);
  for (int r = 0; r < vc->rows; r++) {
    int n = 0;
    int last = -1;
    /* find last non-blank column so we don't log a wall of spaces */
    for (int c = 0; c < vc->cols; c++) {
      uint16_t ch = vc->cells[r * vc->cols + c].ch;
      if (ch != 0 && ch != ' ') {
        last = c;
      }
    }
    if (last < 0) {
      continue; /* skip empty rows */
    }
    for (int c = 0; c <= last && n < (int)sizeof(line) - 1; c++) {
      uint16_t ch = vc->cells[r * vc->cols + c].ch;
      line[n++] = (ch >= 32 && ch < 127) ? (char)ch : ' ';
    }
    line[n] = '\0';
    ESP_LOGI(TAG, "row %2d | %s", r, line);
  }
}

/*
 * Draw the 128-255 pseudographics page (box drawing, shades, blocks, arrows,
 * shapes, card suits -- see components/tty/src/font_builtin.c) as a
 * bordered 16x8 grid, framed with the box-drawing glyphs from that same page.
 */
static void dump_pseudographics(macjr_tty_t *tty) {
  uint8_t row[18];
  static const char crlf[] = "\r\n";

  row[0] = 146; /* down-right corner */
  for (int i = 1; i <= 16; i++) {
    row[i] = 144; /* horizontal */
  }
  row[17] = 147; /* down-left corner */
  macjr_tty_write(tty, (const char *)row, sizeof(row));
  macjr_tty_write(tty, crlf, sizeof(crlf) - 1);

  for (int hi = 8; hi <= 15; hi++) {
    row[0] = 145; /* vertical */
    for (int lo = 0; lo < 16; lo++) {
      row[1 + lo] = (uint8_t)(hi * 16 + lo);
    }
    row[17] = 145; /* vertical */
    macjr_tty_write(tty, (const char *)row, sizeof(row));
    macjr_tty_write(tty, crlf, sizeof(crlf) - 1);
  }

  row[0] = 148; /* up-right corner */
  for (int i = 1; i <= 16; i++) {
    row[i] = 144; /* horizontal */
  }
  row[17] = 149; /* up-left corner */
  macjr_tty_write(tty, (const char *)row, sizeof(row));
  macjr_tty_write(tty, crlf, sizeof(crlf) - 1);
}

void app_main(void) {
  macjr_hal_display_t *hal = display_hal_init();
  if (!hal) {
    ESP_LOGE(TAG, "display_hal_init failed");
    return;
  }

  /* --- init --- */
  macjr_tty_t *tty = macjr_tty_create(hal, 8, 16); /* matches the built-in 8x16 font */
  if (!tty) {
    ESP_LOGE(TAG, "macjr_tty_create failed");
    return;
  }

  struct macjr_winsize *ws = macjr_tty_winsize(tty);
  ESP_LOGI(TAG, "tty up: %ux%u chars on %ux%u px", ws->ws_col, ws->ws_row, ws->ws_xpixel, ws->ws_ypixel);

  /* --- demo: write some output, including a few ANSI SGR color escapes --- */
  const char *banner =
      "MACJr TTY demo\r\n"
      "\x1b[32mgreen\x1b[0m \x1b[31mred\x1b[0m \x1b[1;34mbold blue\x1b[0m\r\n"
      "login: ";
  macjr_tty_write(tty, banner, strlen(banner));

  /* --- demo: simulate keyboard input through the line discipline --- */
  /* With ICANON+ECHO on, injected bytes are echoed and a line is buffered. */
  const char *typed = "root\n";
  macjr_kbd_inject(tty, (const uint8_t *)typed, strlen(typed));

  /* Push everything that changed to the panel. */
  int64_t flush_start_us = esp_timer_get_time();
  macjr_display_flush(tty);
  int64_t flush_us = esp_timer_get_time() - flush_start_us;
  ESP_LOGI(TAG, "display flush took %lld.%03lld ms", flush_us / 1000, flush_us % 1000);

  /* Read the completed line back, the way a shell/app would. */
  char in[64];
  ssize_t got = macjr_tty_read(tty, in, sizeof(in) - 1);
  if (got > 0) {
    in[got] = '\0';
    /* strip trailing newline for clean logging */
    if (got > 0 && in[got - 1] == '\n') {
      in[got - 1] = '\0';
    }
    ESP_LOGI(TAG, "read %d bytes from tty: \"%s\"", (int)got, in);
  } else {
    ESP_LOGW(TAG, "tty read returned %d", (int)got);
  }

  dump_vc(tty);

  /* --- demo: show off the pseudographics page added to the builtin font --- */
  const char *pg_banner = "\r\npseudographics:\r\n";
  macjr_tty_write(tty, pg_banner, strlen(pg_banner));
  dump_pseudographics(tty);
  macjr_display_flush(tty);

  /* --- live: print uptime on the line right after the login prompt, --- */
  /* --- redrawn (invalidated) at 24fps for the lifetime of the device. --- */
  macjr_vc_t *vc = macjr_tty_vc(tty);
  int uptime_row = vc->cursor_r;
  const TickType_t frame_ticks = pdMS_TO_TICKS(1000 / 24);

  while (1) {
    int64_t us = esp_timer_get_time();
    int64_t secs = us / 1000000;
    int64_t ms = (us / 1000) % 1000;

    char line[64];
    int n = snprintf(line, sizeof(line), "\x1b[%d;1H\x1b[Kuptime: %lld.%03llds",
                      uptime_row + 1, secs, ms);
    macjr_tty_write(tty, line, (size_t)n);
    macjr_display_flush(tty);

    vTaskDelay(frame_ticks);
  }
}
