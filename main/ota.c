/*
 * Firmware updates (ota.h). The slots are ota_0 and ota_1 in
 * partitions.csv; otadata records which one boots and whether a new one
 * is still on trial (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
 *
 * An update file is a plain build/MACJr.bin. Before writing it, its app
 * description (the esp_app_desc_t right after the image header and the
 * first segment header) must name this project: the same chip runs other
 * firmwares too (the esp-sdr branch). esp_ota_end() then checks the
 * image's SHA-256. Writing goes to the slot that does not run, so a power
 * cut at any point leaves the running firmware as it was.
 */
#include "ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "keyboard.h"

static const char *TAG = "ota";

#define OTA_CHUNK      4096
#define OTA_BACK_KEY   KEY_MK_VO /* В/О held at power-up: previous firmware */
#define OTA_RESTART_MS 1500      /* to read the last line before the reset */

static const char *state_name(esp_ota_img_states_t st) {
  switch (st) {
  case ESP_OTA_IMG_NEW: return "new";
  case ESP_OTA_IMG_PENDING_VERIFY: return "on trial";
  case ESP_OTA_IMG_VALID: return "valid";
  case ESP_OTA_IMG_INVALID: return "invalid";
  case ESP_OTA_IMG_ABORTED: return "aborted";
  default: return "-";
  }
}

void ota_boot(void) {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_app_desc_t *me = esp_app_get_description();
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;

  esp_ota_get_state_partition(run, &st);
  ESP_LOGI(TAG, "running %s %s from %s (%s)", me->project_name, me->version, run->label, state_name(st));

  if (!keyboard_key_down(OTA_BACK_KEY)) {
    return;
  }
  const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
  esp_app_desc_t desc;
  if (!other || esp_ota_get_partition_description(other, &desc) != ESP_OK) {
    ESP_LOGW(TAG, "В/О held, but there is no other firmware");
    return;
  }
  ESP_LOGW(TAG, "В/О held: back to %s %s in %s", desc.project_name, desc.version, other->label);
  if (st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_invalid_rollback_and_reboot(); /* this one won't boot again */
  } else if (esp_ota_set_boot_partition(other) == ESP_OK) {
    esp_restart();
  }
  ESP_LOGE(TAG, "cannot boot %s", other->label);
}

void ota_confirm(void) {
  esp_ota_img_states_t st;

  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
      st == ESP_OTA_IMG_PENDING_VERIFY) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      ESP_LOGI(TAG, "firmware confirmed: no rollback");
    }
  }
}

/* ---- the update from the SD card ---------------------------------------- */

static macjr_tty_t *s_tty;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void say(const char *fmt, ...) {
  char buf[96];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }
  if (n > (int) sizeof(buf) - 3) {
    n = sizeof(buf) - 3;
  }
  buf[n++] = '\r';
  buf[n++] = '\n';
  ESP_LOGI(TAG, "%.*s", n - 2, buf);
  macjr_tty_write(s_tty, buf, n);
  macjr_display_flush(s_tty);
}

/* Moves the installed (or refused) file out of the way: name it after its
 * version, or `bad` for a file that cannot be installed. */
static void put_away(const char *version, bool bad) {
  char to[96], ver[40];
  size_t k = 0;

  /* a version like "0.0.3-2-gabc" is fine in a FAT long name; '/' is not */
  for (const char *p = version; *p && k < sizeof(ver) - 1; p++) {
    ver[k++] = (*p == '/' || *p == '\\' || *p == ':') ? '_' : *p;
  }
  ver[k] = '\0';
  if (bad) {
    snprintf(to, sizeof(to), "%s/MACJR.BAD", OTA_CARD_DIR);
  } else {
    snprintf(to, sizeof(to), "%s/MACJR-%s.BIN", OTA_CARD_DIR, ver);
  }
  unlink(to);
  if (rename(OTA_CARD_FILE, to) != 0) {
    ESP_LOGW(TAG, "cannot rename %s to %s", OTA_CARD_FILE, to);
    unlink(OTA_CARD_FILE); /* else it would be tried at every boot */
  }
}

/* The app description inside an image file: after the image header and
 * the first segment's header. */
static bool read_desc(FILE *f, esp_app_desc_t *desc) {
  esp_image_header_t hdr;
  esp_image_segment_header_t seg;

  return fread(&hdr, sizeof(hdr), 1, f) == 1 && hdr.magic == ESP_IMAGE_HEADER_MAGIC &&
         hdr.chip_id == ESP_CHIP_ID_ESP32S3 && fread(&seg, sizeof(seg), 1, f) == 1 &&
         fread(desc, sizeof(*desc), 1, f) == 1 && desc->magic_word == ESP_APP_DESC_MAGIC_WORD;
}

void ota_from_card(macjr_tty_t *tty) {
  struct stat sb;

  if (stat(OTA_CARD_FILE, &sb) != 0) {
    return; /* nothing to install */
  }
  s_tty = tty;
  macjr_tty_write(tty, "\x1b[0m\x1b[2J", 8);
  macjr_tty_header(tty, "Firmware update");
  macjr_tty_write(tty, "\x1b[3;1H", 6);

  const esp_app_desc_t *me = esp_app_get_description();
  esp_app_desc_t desc;
  FILE *f = fopen(OTA_CARD_FILE, "rb");
  if (!f) {
    say(" Cannot open MACJR.BIN");
    return;
  }
  if (!read_desc(f, &desc) || strncmp(desc.project_name, me->project_name, sizeof(desc.project_name)) != 0) {
    fclose(f);
    say(" MACJR.BIN is not a MacJr firmware:");
    say(" left as UPDATE/MACJR.BAD");
    put_away("", true);
    vTaskDelay(pdMS_TO_TICKS(3000));
    return;
  }
  if (memcmp(desc.app_elf_sha256, me->app_elf_sha256, sizeof(me->app_elf_sha256)) == 0) {
    fclose(f);
    ESP_LOGI(TAG, "%s is the running firmware (%s)", OTA_CARD_FILE, me->version);
    put_away(desc.version, false);
    return;
  }

  const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
  say(" %.31s -> %.31s", me->version, desc.version);
  if (!slot || (size_t) sb.st_size > slot->size) {
    fclose(f);
    say(" Too big for the slot (%ld bytes)", (long) sb.st_size);
    put_away("", true);
    vTaskDelay(pdMS_TO_TICKS(3000));
    return;
  }
  say(" Writing %ld bytes to %s", (long) sb.st_size, slot->label);

  esp_ota_handle_t h;
  char *buf = malloc(OTA_CHUNK);
  esp_err_t err = buf ? esp_ota_begin(slot, sb.st_size, &h) : ESP_ERR_NO_MEM; /* erases what it needs */
  size_t done = 0;
  int shown = -1;
  while (err == ESP_OK) {
    size_t n = fread(buf, 1, OTA_CHUNK, f);
    if (n == 0) {
      break;
    }
    err = esp_ota_write(h, buf, n);
    done += n;
    int pct = (int) (done * 100 / sb.st_size);
    if (pct / 10 != shown) {
      shown = pct / 10;
      char line[24];
      int k = snprintf(line, sizeof(line), "\r %3d%%", pct);
      macjr_tty_write(tty, line, k);
      macjr_display_flush(tty);
    }
  }
  macjr_tty_write(tty, "\r\n", 2);
  if (err == ESP_OK && done != (size_t) sb.st_size) {
    err = ESP_ERR_INVALID_SIZE; /* a read error */
  }
  if (err == ESP_OK) {
    err = esp_ota_end(h); /* checks the image and its SHA-256 */
  } else if (buf) {
    esp_ota_abort(h);
  }
  free(buf);
  fclose(f);

  if (err == ESP_OK) {
    err = esp_ota_set_boot_partition(slot);
  }
  if (err != ESP_OK) {
    say(" Failed: %s", esp_err_to_name(err));
    say(" Left as UPDATE/MACJR.BAD");
    put_away("", true);
    vTaskDelay(pdMS_TO_TICKS(3000));
    return;
  }
  put_away(desc.version, false);
  say(" Done. Restarting...");
  vTaskDelay(pdMS_TO_TICKS(OTA_RESTART_MS));
  esp_restart();
}
