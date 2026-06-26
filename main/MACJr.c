/*
 * MACJr demo entry point: brings up the ST7789-backed display HAL and the
 * macjr_tty TTY stack, then draws a single static screen -- a recreation of
 * a classic Turbo C IDE -- and leaves it on screen.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "tty/tty.h"
#include "display_hal.h"

static const char *TAG = "macjr_demo";

/* Position the cursor, set an SGR fg;bg pair, then write raw bytes (may
 * include 128-255 pseudographics glyphs, not just ASCII). 1-indexed row/col
 * to match the CUP escape (\x1b[row;colH). */
static void put(macjr_tty_t *tty, int row, int col, const char *sgr, const void *data, size_t len) {
  char esc[32];
  int n = snprintf(esc, sizeof(esc), "\x1b[%d;%dH\x1b[%sm", row, col, sgr);
  macjr_tty_write(tty, esc, (size_t)n);
  macjr_tty_write(tty, data, len);
}

/* Console is a fixed 40x15 grid; the layout below is authored directly at
 * that size -- no design/view scaling. */
#define IDE_COLS 40
#define IDE_ROWS 15

/* Double- and single-line box-drawing glyphs, CP737 layout
 * (components/tty/src/font_builtin.c). The editor window uses the double
 * line; the "Linking" dialog uses the single line, like the original. */
#define DBL_TL 201 /* ╔ */
#define DBL_TR 187 /* ╗ */
#define DBL_BL 200 /* ╚ */
#define DBL_BR 188 /* ╝ */
#define DBL_H 205  /* ═ */
#define DBL_V 186  /* ║ */
#define SGL_TL 218 /* ┌ */
#define SGL_TR 191 /* ┐ */
#define SGL_BL 192 /* └ */
#define SGL_BR 217 /* ┘ */
#define SGL_H 196  /* ─ */
#define SGL_V 179  /* │ */

/* Left-pad/truncate text to an exact field width with spaces, then put(). */
static void field_at(macjr_tty_t *tty, int row, int col, const char *sgr, int width, const char *text) {
  char field[IDE_COLS + 1];
  snprintf(field, sizeof(field), "%-*.*s", width, width, text);
  put(tty, row, col, sgr, field, (size_t)width);
}

/* Draw a horizontal border line (corner, h-line glyphs, corner) `width`
 * cells wide starting at (row, col), with an optional label centered into
 * the line (e.g. a window's title tab). */
static void border_row(macjr_tty_t *tty, int row, int col, int width, const char *sgr, uint8_t left,
                        uint8_t right, uint8_t h, const char *label_sgr, const char *label) {
  uint8_t buf[IDE_COLS];
  buf[0] = left;
  memset(buf + 1, h, (size_t)(width - 2));
  buf[width - 1] = right;
  put(tty, row, col, sgr, buf, (size_t)width);
  if (label && *label) {
    int len = (int)strlen(label);
    int off = (width - 2 - len) / 2;
    put(tty, row, col + 1 + off, label_sgr, label, (size_t)len);
  }
}

/*
 * One-shot, static recreation of a classic Turbo C IDE screen, authored
 * directly at the console's fixed 40x15 size: a top menu bar, a
 * double-line-bordered editor window with an "Edit" tab centered in its
 * top border, and a single-line-bordered "Linking" status dialog centered
 * on top of it. There is no animation or redraw loop, it is drawn once and
 * left on screen.
 */
static void dump_turboc_ide(macjr_tty_t *tty) {
  static const char *MENU_SGR = "97;44";       /* bright white on blue: menu bar */
  static const char *BORDER_SGR = "96;40";     /* cyan on black: window border */
  static const char *TAB_SGR = "30;46";        /* black on cyan: "Edit" tab */
  static const char *YELLOW_SGR = "93;40";     /* bright yellow on black: code */
  static const char *WHITE_SGR = "97;40";      /* bright white on black: braces */
  static const char *DLG_BORDER_SGR = "30;42"; /* black on green: dialog frame */
  static const char *DLG_TEXT_SGR = "96;40";   /* cyan on black: dialog fields */
  static const char *DLG_OK_SGR = "97;44";     /* white on blue: "Success" bar */

  /* row 1: top menu bar */
  field_at(tty, 1, 1, MENU_SGR, IDE_COLS, "File Edit Run Compile Options");

  /* row 2: editor window top border, "Edit" tab centered in it */
  border_row(tty, 2, 1, IDE_COLS, BORDER_SGR, DBL_TL, DBL_TR, DBL_H, TAB_SGR, "Edit");

  /* rows 3-14: editor body -- side borders plus a tiny C source file */
  const struct {
    const char *sgr;
    const char *text;
  } body[IDE_ROWS - 3] = {
      {YELLOW_SGR, " #include <stdio.h>"},
      {YELLOW_SGR, ""},
      {YELLOW_SGR, " int main(int argc, char *argv[])"},
      {WHITE_SGR, " {"},
      {YELLOW_SGR, "     printf(\"hello\\n\");"},
      {YELLOW_SGR, ""},
      {YELLOW_SGR, ""},
      {YELLOW_SGR, ""},
      {YELLOW_SGR, ""},
      {YELLOW_SGR, ""},
      {WHITE_SGR, " }"},
      {YELLOW_SGR, ""},
  };
  for (int i = 0; i < IDE_ROWS - 3; i++) {
    int row = 3 + i;
    put(tty, row, 1, BORDER_SGR, &(uint8_t){DBL_V}, 1);
    field_at(tty, row, 2, body[i].sgr, IDE_COLS - 2, body[i].text);
    put(tty, row, IDE_COLS, BORDER_SGR, &(uint8_t){DBL_V}, 1);
  }

  /* row 15: editor window bottom border */
  border_row(tty, IDE_ROWS, 1, IDE_COLS, BORDER_SGR, DBL_BL, DBL_BR, DBL_H, NULL, NULL);

  /*
   * "Linking" build-status dialog: single-line border, centered over the
   * editor window, painted last so it overlaps whatever code text is
   * underneath it -- same as a real modal dialog popping up mid-build.
   */
  enum { DLG_W = 22, DLG_H = 7 };
  int dlg_col = (IDE_COLS - DLG_W) / 2 + 1;
  int dlg_row = (IDE_ROWS - DLG_H) / 2 + 1;

  border_row(tty, dlg_row, dlg_col, DLG_W, DLG_BORDER_SGR, SGL_TL, SGL_TR, SGL_H, DLG_BORDER_SGR, "Linking");

  uint32_t flash_bytes = 0;
  esp_flash_get_size(NULL, &flash_bytes);
  char flash_line[20];
  snprintf(flash_line, sizeof(flash_line), "Flash: %luK", (unsigned long)(flash_bytes / 1024));

  char mem_line[20];
  snprintf(mem_line, sizeof(mem_line), "Mem: %luK", (unsigned long)(esp_get_free_heap_size() / 1024));

  const char *dlg_lines[] = {
      "EXE: C:\\NONAME.EXE",
      "Link: CS.LIB",
      flash_line,
      mem_line,
  };
  for (int i = 0; i < 4; i++) {
    int row = dlg_row + 1 + i;
    put(tty, row, dlg_col, DLG_BORDER_SGR, &(uint8_t){SGL_V}, 1);
    field_at(tty, row, dlg_col + 1, DLG_TEXT_SGR, DLG_W - 2, dlg_lines[i]);
    put(tty, row, dlg_col + DLG_W - 1, DLG_BORDER_SGR, &(uint8_t){SGL_V}, 1);
  }

  int ok_row = dlg_row + 5;
  put(tty, ok_row, dlg_col, DLG_BORDER_SGR, &(uint8_t){SGL_V}, 1);
  field_at(tty, ok_row, dlg_col + 1, DLG_OK_SGR, DLG_W - 2, "Success");
  put(tty, ok_row, dlg_col + DLG_W - 1, DLG_BORDER_SGR, &(uint8_t){SGL_V}, 1);

  border_row(tty, dlg_row + DLG_H - 1, dlg_col, DLG_W, DLG_BORDER_SGR, SGL_BL, SGL_BR, SGL_H, NULL, NULL);
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

  /* Static, one-shot recreation of a classic Turbo C IDE screen. No
   * keyboard input, no redraw loop -- hide the cursor, draw it once, flush
   * once, and leave it on screen. */
  const char *clear = "\x1b[2J\x1b[?25l";
  macjr_tty_write(tty, clear, strlen(clear));
  dump_turboc_ide(tty);

  int64_t flush_start_us = esp_timer_get_time();
  macjr_display_flush(tty);
  int64_t flush_us = esp_timer_get_time() - flush_start_us;
  ESP_LOGI(TAG, "display flush took %lld.%03lld ms", flush_us / 1000, flush_us % 1000);
}
