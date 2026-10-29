/*
 * Firmware entry point: brings up the board HALs (SD card, keyboard, ST7789
 * display), asks the boot menu which mode to start (see components/boot),
 * then runs it:
 *  - TTY demo: macjr_tty stack + static Turbo C IDE screen (ide_screen.c)
 *  - SDR + UAC: Si5351 I/Q LO for the Tayloe detector, ADC I/Q streamed
 *    to the host as a USB UAC microphone
 *  - MK-52: Elektronika MK-52 calculator on the MK-52 keyboard (never returns)
 *  - USB drive: the SD card exposed to the host as a USB drive (never returns)
 *  - Port: the port layout picture IMG/layout.png from the SD card
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tty/tty.h"
#include "boot.h"
#include "display_hal.h"
#include "ide_screen.h"
#include "sdcard.h"
#include "matmul_bench.h"
#include "keyboard.h"
#include "speaker.h"
#include "mic.h"
#include "sdr.h"
#include "monitor.h"
#include "mk52.h"
#include "usbdrive.h"
#include "port.h"

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

static void run_tty_demo(macjr_tty_t *tty) {
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

/* UAC takes the native USB port away from USB-Serial-JTAG, so this mode is
 * only entered from the boot menu; the default boot keeps flashing/console
 * on that port working. */
static void run_sdr_uac(macjr_tty_t *tty) {
  const char *clear = "\x1b[2J\x1b[?25l";
  macjr_tty_write(tty, clear, strlen(clear));

  const char *status;
  if (sdr_init() != ESP_OK) {
    status = "SDR: Si5351 init failed\r\n";
  } else if (mic_init() != ESP_OK || mic_uac_start() != ESP_OK) {
    status = "SDR: LO 7.055 MHz I/Q\r\nUAC: mic init failed\r\n";
  } else {
    status = "SDR: LO 7.055 MHz I/Q\r\nUAC: streaming I/Q as USB mic\r\n";
  }
  ESP_LOGI(TAG, "%s", status);
  macjr_tty_write(tty, status, strlen(status));
  macjr_display_flush(tty);
}

static void start_monitor(macjr_tty_t *tty) {
  if (monitor_start(tty) != ESP_OK) {
    ESP_LOGW(TAG, "monitor_start failed");
  }
}

void app_main(void) {
  if (sdcard_init() != ESP_OK) {
    ESP_LOGW(TAG, "sdcard_init failed, continuing without SD card");
  }

  if (speaker_init() != ESP_OK) {
    ESP_LOGW(TAG, "speaker_init failed, continuing without sound");
  }

  if (keyboard_init() != ESP_OK) {
    ESP_LOGW(TAG, "keyboard init failed, continuing without keyboard");
  } else {
    xTaskCreate(kbrd_task, "kbrd", 3072, NULL, 4, NULL);
  }

  macjr_hal_display_t *display = display_hal_init();
  if (!display) {
    ESP_LOGE(TAG, "display_hal_init failed");
    return;
  }

  boot_mode_t mode = boot_select_mode(display);

  macjr_tty_t *tty = macjr_tty_create(display, 8, 16); /* matches the built-in 8x16 font */
  if (!tty) {
    ESP_LOGE(TAG, "macjr_tty_create failed");
    return;
  }

  struct macjr_winsize *ws = macjr_tty_winsize(tty);
  ESP_LOGI(TAG, "tty up: %ux%u chars on %ux%u px", ws->ws_col, ws->ws_row, ws->ws_xpixel, ws->ws_ypixel);

  /* The monitor (key Cx) runs in every mode except the calculator, which
   * needs Cx itself. Apps that don't return start it first. */
  switch (mode) {
  case BOOT_MODE_SDR_UAC:
    run_sdr_uac(tty);
    break;
  case BOOT_MODE_MK52:
    mk52_run(tty, display); /* never returns */
    break;
  case BOOT_MODE_USB_DRIVE:
    start_monitor(tty);
    usbdrive_run(tty); /* never returns */
    break;
  case BOOT_MODE_PORT:
    port_show(tty, display);
    monitor_set_redraw_cb(port_redraw, NULL); /* the picture is not in the tty */
    break;
  case BOOT_MODE_TTY_DEMO:
  default:
    run_tty_demo(tty);
    break;
  }

  /* Started after the mode has drawn its screen, so the monitor saves and
   * later restores that screen. */
  start_monitor(tty);
}
