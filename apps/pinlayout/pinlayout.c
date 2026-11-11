/*
 * PORT.ELF -- the board's port layout picture: decode the PNG row by row
 * and draw it scaled to fit. One of the agreed exceptions to console-only
 * apps: it draws on the display HAL behind the tty.
 *
 * Scaling is nearest-neighbor and streams with the decoder: target row ty
 * shows source row ty * h / th, so as source rows arrive in order each one
 * is drawn zero times (shrinking), once, or several times (enlarging).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "macjr.h"
#include "monitor.h"
#include "png.h"
#include "sdcard.h"

/* The picture shown: the board's port layout. */
#define PORT_IMAGE_PATH SDCARD_MOUNT_POINT "/IMG/layout.png"

static const char *TAG = "port";

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

static void port_show(macjr_tty_t *tty, macjr_hal_display_t *display) {
  s_tty = tty;
  s_display = display;
  view_t v = {.d = display};
  esp_err_t ret = display->blit_rgb888 ? png_decode_file(PORT_IMAGE_PATH, on_header, on_row, &v)
                                       : ESP_ERR_NOT_SUPPORTED;
  if (display->end_frame) display->end_frame(display);
  free(v.line);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "%s: %s", PORT_IMAGE_PATH, esp_err_to_name(ret));
    char msg[160];
    snprintf(msg, sizeof(msg), "\x1b[2J\x1b[?25l\x1b[1;1H\x1b[91mCannot show image:\r\n\x1b[97m%s\r\n%s\r\n",
             PORT_IMAGE_PATH, ret == ESP_ERR_NOT_FOUND ? "file not found" : esp_err_to_name(ret));
    macjr_tty_write(tty, msg, strlen(msg));
    macjr_display_flush(tty);
  }
}

/* The picture is not part of the tty: draw it again after the monitor. */
static void port_redraw(void *ctx) {
  (void)ctx;
  if (s_display) {
    port_show(s_tty, s_display);
  }
}

/* Show the picture until Cx, then return to the menu. */
int main(int argc, char *argv[], macjr_tty_t *tty) {
  port_show(tty, macjr_tty_display(tty));
  monitor_set_redraw_cb(port_redraw, NULL);
  while (sys_getkey() != SYS_KEY_ESC) {
  }
  monitor_set_redraw_cb(NULL, NULL); /* the callback goes away with us */
  return 0;
}
