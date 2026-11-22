/*
 * IMAGEVIEWER.ELF -- show a PNG or JPEG image, given its path as argv[1]. The
 * navigator launches it when В↑ is pressed on an image file, the way a .py
 * launches UPYTHON and a sound launches AUDIOPLAYER.
 *
 * It decodes the image row by row and draws it scaled to fit, keeping the
 * aspect ratio. One of the agreed exceptions to console-only apps: it draws on
 * the display HAL behind the tty. Cx returns to the navigator. The system
 * monitor and its notices repaint the display from their own tty, so the
 * picture is drawn again each time they close.
 *
 * Scaling is nearest-neighbor and streams with the decoder: target row ty
 * shows source row ty * h / th, so as source rows arrive in order each one is
 * drawn zero times (shrinking), once, or several times (enlarging).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "esp_log.h"
#include "keyboard.h"
#include "macjr.h"
#include "monitor.h"
#include "png.h"

static const char *TAG = "pngview";

typedef struct {
  macjr_hal_display_t *d;
  uint32_t w, h;   /* source image */
  uint32_t tw, th; /* on screen */
  int ox, oy;      /* top-left on screen */
  uint32_t next_ty;
  uint8_t *line;   /* tw * 3 */
} view_t;

static bool on_header(void *ctx, uint32_t w, uint32_t h) {
  view_t *v = ctx;
  macjr_hal_display_t *d = v->d;
  v->w = w;
  v->h = h;
  /* fit inside the screen, keeping the aspect ratio */
  if ((uint64_t)w * d->height_px > (uint64_t)h * d->width_px) {
    v->tw = d->width_px;
    v->th = (uint32_t)((uint64_t)h * d->width_px / w);
  } else {
    v->th = d->height_px;
    v->tw = (uint32_t)((uint64_t)w * d->height_px / h);
  }
  if (v->tw == 0) v->tw = 1;
  if (v->th == 0) v->th = 1;
  v->ox = (d->width_px - (int)v->tw) / 2;
  v->oy = (d->height_px - (int)v->th) / 2;
  v->line = malloc(v->tw * 3);
  if (!v->line) {
    return false;
  }
  if (d->begin_frame) d->begin_frame(d);
  d->fill_rect(d, 0, 0, d->width_px, d->height_px, 0x000000);
  ESP_LOGI(TAG, "%ux%u image shown at %ux%u", (unsigned)w, (unsigned)h, (unsigned)v->tw, (unsigned)v->th);
  return true;
}

static void on_row(void *ctx, uint32_t y, const uint8_t *rgb) {
  view_t *v = ctx;
  while (v->next_ty < v->th && (uint64_t)v->next_ty * v->h / v->th == y) {
    for (uint32_t tx = 0; tx < v->tw; tx++) {
      memcpy(&v->line[tx * 3], &rgb[(uint64_t)tx * v->w / v->tw * 3], 3);
    }
    v->d->blit_rgb888(v->d, v->ox, v->oy + (int)v->next_ty, (int)v->tw, 1, v->line);
    v->next_ty++;
  }
}

static macjr_tty_t *s_tty;
static macjr_hal_display_t *s_display;
static const char *s_path;

/* Decode by extension: JPEG via the ROM TJpgDec wrapper, everything else as
 * PNG. (The loader exports no strcasecmp, so match the suffix by hand.) */
static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static bool ends_with_ci(const char *s, const char *suffix) {
  size_t ls = strlen(s), lx = strlen(suffix);
  if (ls < lx) {
    return false;
  }
  for (size_t i = 0; i < lx; i++) {
    if (lower(s[ls - lx + i]) != lower(suffix[i])) {
      return false;
    }
  }
  return true;
}

static bool is_jpeg(const char *path) {
  return ends_with_ci(path, ".jpg") || ends_with_ci(path, ".jpeg");
}

static void show_image(macjr_tty_t *tty, macjr_hal_display_t *display) {
  s_tty = tty;
  s_display = display;
  view_t v = {.d = display};
  esp_err_t ret;
  if (!display->blit_rgb888) {
    ret = ESP_ERR_NOT_SUPPORTED;
  } else if (is_jpeg(s_path)) {
    ret = jpeg_decode_file(s_path, on_header, on_row, &v);
  } else {
    ret = png_decode_file(s_path, on_header, on_row, &v);
  }
  if (display->end_frame) display->end_frame(display);
  free(v.line);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "%s: %s", s_path, esp_err_to_name(ret));
    char msg[256];
    snprintf(msg, sizeof(msg), "\x1b[2J\x1b[?25l\x1b[1;1H\x1b[91mCannot show image:\r\n\x1b[97m%s\r\n%s\r\n",
             s_path, ret == ESP_ERR_NOT_FOUND ? "file not found" : esp_err_to_name(ret));
    macjr_tty_write(tty, msg, strlen(msg));
    macjr_display_flush(tty);
  }
}

#define POLL_MS 100 /* how soon the picture is back after the monitor */

/* Wait for Cx; draw the picture again whenever the monitor has been on
 * screen (it isn't part of the tty, which is what comes back). */
static void wait_esc(void) {
  int fd = open(KBD_DEV_PATH, O_RDONLY);
  if (fd < 0) {
    while (sys_getkey() != SYS_KEY_ESC) {
    }
    return;
  }
  bool was_open = false;
  for (;;) {
    fd_set keys;
    FD_ZERO(&keys);
    FD_SET(fd, &keys);
    struct timeval timeout = {.tv_sec = 0, .tv_usec = POLL_MS * 1000};
    uint8_t key = 0;
    bool got = select(fd + 1, &keys, NULL, NULL, &timeout) > 0 && read(fd, &key, 1) == 1;
    bool open_now = monitor_is_open();
    if (was_open && !open_now) {
      show_image(s_tty, s_display);
    }
    was_open = open_now;
    if (got && !open_now && key == KEY_ESC) {
      break;
    }
  }
  close(fd);
}

/* Show the picture until Cx, then return to the navigator. */
int main(int argc, char *argv[], macjr_tty_t *tty) {
  if (argc < 2 || !argv[1][0]) {
    const char *msg = "\x1b[2J\x1b[?25l\x1b[1;1H\x1b[91mIMAGEVIEWER: no file given\r\n";
    macjr_tty_write(tty, msg, strlen(msg));
    macjr_display_flush(tty);
    while (sys_getkey() != SYS_KEY_ESC) {
    }
    return 1;
  }
  s_path = argv[1];

  show_image(tty, macjr_tty_display(tty));
  wait_esc();
  return 0;
}
