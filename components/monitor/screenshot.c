/*
 * Screenshot: what the panel shows, saved as a PNG on the SD card. Runs in
 * the monitor task, so the program on screen goes on meanwhile; the image
 * is copied first (screenshot_grab), so neither what the program draws next
 * nor the monitor's own "Taking screenshot" window ends up in the file.
 */
#include "monitor.h"

#include <stdio.h>
#include <string.h>
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

/* Log `text` as a failure and hand it to the caller. */
static bool fail(char *msg, size_t len, const char *text) {
  ESP_LOGW(TAG, "screenshot: %s", text);
  snprintf(msg, len, "%s", text);
  return false;
}

uint8_t *screenshot_grab(macjr_tty_t *tty, char *msg, size_t len) {
  macjr_hal_display_t *d = macjr_tty_display(tty);
  if (!d->read_rgb888) {
    fail(msg, len, "Display can't be read");
    return NULL;
  }
  uint8_t *rgb = heap_caps_malloc((size_t)d->width_px * d->height_px * 3, MALLOC_CAP_SPIRAM);
  if (!rgb) {
    fail(msg, len, "Out of memory");
    return NULL;
  }
  d->read_rgb888(d, rgb);
  return rgb;
}

bool screenshot_write(macjr_tty_t *tty, uint8_t *rgb, char *msg, size_t len) {
  macjr_hal_display_t *d = macjr_tty_display(tty);
  struct stat st;
  if (stat(SHOT_DIR, &st) != 0 && mkdir(SHOT_DIR, 0775) != 0) {
    heap_caps_free(rgb);
    return fail(msg, len, "No SD card");
  }
  char path[64];
  if (!next_path(path, sizeof(path))) {
    heap_caps_free(rgb);
    return fail(msg, len, "SHOTS is full");
  }
  esp_err_t err = png_write_file(path, d->width_px, d->height_px, rgb);
  heap_caps_free(rgb);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "saving %s failed: %s", path, esp_err_to_name(err));
    return fail(msg, len, "Writing the file failed");
  }
  ESP_LOGI(TAG, "saved %s", path);
  snprintf(msg, len, "%s", path + strlen(SDCARD_MOUNT_POINT) + 1); /* SHOTS/SHOTnnnn.PNG */
  return true;
}
