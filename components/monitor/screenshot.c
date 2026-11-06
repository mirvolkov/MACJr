/*
 * Screenshot: what the panel shows, saved as a PNG on the SD card. Runs in
 * the monitor task, so the program on screen goes on meanwhile; the image
 * is copied first, so what it draws next doesn't end up in the file.
 */
#include "monitor.h"

#include <stdio.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "png.h"
#include "sdcard.h"

#define SHOT_DIR SDCARD_MOUNT_POINT "/SHOTS"
#define SHOT_MAX 9999

static const char *TAG = "screenshot";

/* First free SHOT_DIR/SHOTnnnn.PNG; false if there is none. */
static bool next_path(char *path, size_t len) {
  static int s_next = 1; /* no need to walk the taken ones again */
  struct stat st;
  for (; s_next <= SHOT_MAX; s_next++) {
    snprintf(path, len, SHOT_DIR "/SHOT%04d.PNG", s_next);
    if (stat(path, &st) != 0) {
      return true;
    }
  }
  return false;
}

void screenshot_save(macjr_tty_t *tty) {
  macjr_hal_display_t *d = macjr_tty_display(tty);
  if (!d->read_rgb888) {
    ESP_LOGW(TAG, "the display keeps no copy of its picture");
    return;
  }
  struct stat st;
  if (stat(SHOT_DIR, &st) != 0 && mkdir(SHOT_DIR, 0775) != 0) {
    ESP_LOGW(TAG, "cannot create %s: no SD card?", SHOT_DIR);
    return;
  }
  char path[64];
  if (!next_path(path, sizeof(path))) {
    ESP_LOGW(TAG, "%s is full", SHOT_DIR);
    return;
  }
  uint8_t *rgb = heap_caps_malloc((size_t)d->width_px * d->height_px * 3, MALLOC_CAP_SPIRAM);
  if (!rgb) {
    ESP_LOGW(TAG, "no memory for the image");
    return;
  }
  d->read_rgb888(d, rgb);
  esp_err_t err = png_write_file(path, d->width_px, d->height_px, rgb);
  heap_caps_free(rgb);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "saved %s", path);
  } else {
    ESP_LOGW(TAG, "saving %s failed: %s", path, esp_err_to_name(err));
  }
}
