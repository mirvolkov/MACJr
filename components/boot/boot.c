/*
 * Boot menu: a single static screen drawn with the display HAL primitives
 * and the tty's built-in 8x16 bold font. The title uses the same font
 * scaled 3/2 (nearest neighbor) to 12x24, since there is no 24 px font yet.
 *
 * Text lines are kept within 40 columns of 8 px (320 px panel).
 */
#include "boot.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "keyboard.h"

extern const uint8_t macjr_font_builtin_8x16[256][16];

static const char *TAG = "boot";

#define FONT_W 8
#define FONT_H 16
#define TITLE_W 12 /* 8x16 scaled by 3/2 */
#define TITLE_H 24

#define BAND_H 16 /* display HAL buffers at most 40 rows per draw call */

#define COLOR_BG     0x000000
#define COLOR_TITLE  0x55FF55
#define COLOR_TEXT   0xFFFFFF
#define COLOR_HINT   0x808080
#define COLOR_RULE   0x0000AA

static void fill(macjr_hal_display_t *d, int x, int y, int w, int h, uint32_t rgb) {
  for (int yy = y; yy < y + h; yy += BAND_H) {
    int bh = (y + h - yy) < BAND_H ? (y + h - yy) : BAND_H;
    d->fill_rect(d, x, yy, w, bh, rgb);
  }
}

static void draw_text(macjr_hal_display_t *d, int x, int y, const char *s, uint32_t fg) {
  for (; *s; s++, x += FONT_W) {
    d->blit_1bpp_mask(d, x, y, FONT_W, FONT_H, 1, macjr_font_builtin_8x16[(uint8_t)*s], fg, COLOR_BG);
  }
}

/* 8x16 glyph scaled to 12x24: output pixel (x, y) samples source (2x/3, 2y/3). */
static void draw_title(macjr_hal_display_t *d, int x, int y, const char *s, uint32_t fg) {
  uint8_t big[TITLE_H][2];
  for (; *s; s++, x += TITLE_W) {
    const uint8_t *g = macjr_font_builtin_8x16[(uint8_t)*s];
    memset(big, 0, sizeof(big));
    for (int yy = 0; yy < TITLE_H; yy++) {
      uint8_t src = g[yy * 2 / 3];
      for (int xx = 0; xx < TITLE_W; xx++) {
        if (src & (0x80 >> (xx * 2 / 3))) {
          big[yy][xx >> 3] |= 0x80 >> (xx & 7);
        }
      }
    }
    d->blit_1bpp_mask(d, x, y, TITLE_W, TITLE_H, 2, &big[0][0], fg, COLOR_BG);
  }
}

static void draw_menu(macjr_hal_display_t *d) {
  char title[40];
  snprintf(title, sizeof(title), "MacJr %s", esp_app_get_description()->version);
  int title_x = (d->width_px - (int)strlen(title) * TITLE_W) / 2;

  if (d->begin_frame) d->begin_frame(d);
  fill(d, 0, 0, d->width_px, d->height_px, COLOR_BG);
  draw_title(d, title_x < 0 ? 0 : title_x, 8, title, COLOR_TITLE);
  fill(d, 8, 40, d->width_px - 16, 2, COLOR_RULE);
  draw_text(d, 16, 56, "1 - Load TTY demo", COLOR_TEXT);
  draw_text(d, 16, 80, "2 - Init SDR with UAC output", COLOR_TEXT);
  draw_text(d, 16, 104, "3 - MK-52 calculator", COLOR_TEXT);
  draw_text(d, 16, 128, "4 - SD card as USB drive", COLOR_TEXT);
  draw_text(d, 16, 152, "5 - Port layout", COLOR_TEXT);
  draw_text(d, 16, d->height_px - 24, "Select mode: press 1-5", COLOR_HINT);
  if (d->end_frame) d->end_frame(d);
}

static boot_mode_t wait_choice(void) {
  int fd = open(KBD_DEV_PATH, O_RDONLY);
  if (fd < 0) {
    ESP_LOGE(TAG, "open %s failed, using default mode", KBD_DEV_PATH);
    return BOOT_MODE_TTY_DEMO;
  }
  boot_mode_t mode = BOOT_MODE_TTY_DEMO;
  for (;;) {
    uint8_t key;
    if (read(fd, &key, 1) != 1) {
      continue;
    }
    if (key == BOOT_MENU_KEY_1) {
      mode = BOOT_MODE_TTY_DEMO;
      break;
    }
    if (key == BOOT_MENU_KEY_2) {
      mode = BOOT_MODE_SDR_UAC;
      break;
    }
    if (key == BOOT_MENU_KEY_3) {
      mode = BOOT_MODE_MK52;
      break;
    }
    if (key == BOOT_MENU_KEY_4) {
      mode = BOOT_MODE_USB_DRIVE;
      break;
    }
    if (key == BOOT_MENU_KEY_5) {
      mode = BOOT_MODE_PORT;
      break;
    }
    ESP_LOGI(TAG, "key %u is not a menu key", key);
  }
  close(fd);
  return mode;
}

boot_mode_t boot_select_mode(macjr_hal_display_t *display) {
  draw_menu(display);
  boot_mode_t mode = wait_choice();
  ESP_LOGI(TAG, "boot mode %d selected", mode);
  return mode;
}
