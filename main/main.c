/*
 * Firmware entry point: brings up the board HALs (SD card, keyboard, IR
 * receiver, ST7789 display), starts the system monitor (key Cx), then loops over the boot
 * menu (components/boot) and the program picked there: one from /BIN on
 * the SD card, loaded and run by components/exec (apps/: IDE demo, MK-52
 * calculator, SoftRock SDR radio, USB drive, port layout picture, remote
 * control, hello). A program can also be built into the firmware while
 * it is developed: pass it to boot_select() as a boot_builtin_t.
 * В↑ goes back to the menu when a program ends; programs that never return
 * (radio, USB drive, MK-52) stay until the next reboot.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tty/tty.h"
#include "boot.h"
#include "display_hal.h"
#include "sdcard.h"
#include "matmul_bench.h"
#include "keyboard.h"
#include "speaker.h"
#include "db.h"
#include "monitor.h"
#include "exec.h"
#include "ir_rx.h"
#include "ir_tx.h"

static const char *TAG = "MACJR";

#define KEY_BEEP_HZ 2000
#define KEY_BEEP_MS 50

/* Reads /dev/kbrd forever: beeps on every key press and (DEBUG ONLY) logs
 * the key codes. */
static void kbrd_task(void *arg) {
  int fd = open(KBD_DEV_PATH, O_RDONLY);
  if (fd < 0) {
    ESP_LOGE(TAG, "open %s failed: errno %d", KBD_DEV_PATH, errno);
    vTaskDelete(NULL);
    return;
  }
  for (;;) {
    uint8_t keys[8];
    ssize_t n = read(fd, keys, sizeof(keys)); /* blocks until a key is pressed */
    if (n < 0) {
      ESP_LOGW(TAG, "read %s failed: errno %d", KBD_DEV_PATH, errno);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    /* one beep per read: keys pressed in the same instant share it */
    speaker_beep(KEY_BEEP_HZ, KEY_BEEP_MS);
    for (ssize_t i = 0; i < n; i++) {
      ESP_LOGI(TAG, "%s: key %u", KBD_DEV_PATH, keys[i]); /* DEBUG ONLY */
    }
  }
}

static void start_monitor(macjr_tty_t *tty) {
  if (monitor_start(tty) != ESP_OK) {
    ESP_LOGW(TAG, "monitor_start failed");
  }
}

void app_main(void) {
  if (sdcard_init() != ESP_OK) {
    ESP_LOGW(TAG, "sdcard_init failed, continuing without SD card");
  } else if (db_init() != ESP_OK) {
    ESP_LOGW(TAG, "db_init failed, continuing without config");
  }

  if (speaker_init() != ESP_OK) {
    ESP_LOGW(TAG, "speaker_init failed, continuing without sound");
  }

  if (keyboard_init() != ESP_OK) {
    ESP_LOGW(TAG, "keyboard init failed, continuing without keyboard");
  } else {
    xTaskCreate(kbrd_task, "kbrd", 3072, NULL, 4, NULL);
  }

  if (ir_rx_init() != ESP_OK) {
    ESP_LOGW(TAG, "ir_rx_init failed, continuing without IR receiver");
  }
  if (ir_tx_init() != ESP_OK) {
    ESP_LOGW(TAG, "ir_tx_init failed, continuing without IR transmitter");
  }

  macjr_hal_display_t *display = display_hal_init();
  if (!display) {
    ESP_LOGE(TAG, "display_hal_init failed");
    return;
  }

  macjr_tty_t *tty = macjr_tty_create(display, 8, 16); /* matches the built-in 8x16 font */
  if (!tty) {
    ESP_LOGE(TAG, "macjr_tty_create failed");
    return;
  }

  struct macjr_winsize *ws = macjr_tty_winsize(tty);
  ESP_LOGI(TAG, "tty up: %ux%u chars on %ux%u px", ws->ws_col, ws->ws_row, ws->ws_xpixel, ws->ws_ypixel);

  start_monitor(tty);

  for (;;) {
    char path[64];
    const boot_builtin_t *builtin;
    boot_select(tty, NULL, 0, path, sizeof(path), &builtin); /* no built-in programs now */
    if (builtin) {
      exec_launch_main(tty, builtin->name, builtin->main);
    } else {
      exec_launch(tty, path);
    }
  }
}
