/*
 * Firmware entry point: brings up the board HALs (SD card, keyboard, IR
 * receiver, ST7789 display), sets the time zone, and (when the user presses
 * F+БП) keeps the Wi-Fi network saved by the WIFI program joined in the
 * background, with the clock then set over NTP -- the radio and its buffers
 * stay off until asked for. It starts the system monitor (keys F+K), then loops over the
 * file navigator (components/boot) and the program picked there: an ELF
 * file on the SD card, loaded and run by components/exec, a .py script, run
 * by /BIN/UPYTHON.ELF, or a .c source, compiled by /BIN/CC.ELF (apps/: MK-52
 * calculator, USB drive, image viewer, IR receiver and sender, Si5351
 * generator, Wi-Fi, mixer, C compiler, games). A program can also be built into
 * the firmware while it is developed: pass it to boot_select() as a
 * boot_builtin_t. The navigator comes back as soon as a program ends; F and
 * Cx together kill a program, which is how to leave those that never
 * return (radio, USB drive, MK-52).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
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
#include "logdev.h"
#include "monitor.h"
#include "exec.h"
#include "ttydev.h"
#include "ir_rx.h"
#include "ir_tx.h"
#include "net.h"

static const char *TAG = "MACJR";

#define KEY_BEEP_HZ 2000
#define KEY_BEEP_MS 50

/* Does the user want Wi-Fi on? Toggled by F+БП (through the monitor); the
 * Wi-Fi task follows it. Off at boot, so the radio and its ~60 KB of buffers
 * cost nothing until asked for. */
static volatile bool s_wifi_want;

/* Reads /dev/kbrd forever: beeps on every key press. */
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
  }
}

/* Time zone: the db key TIME_DB_KEY_TZ (a POSIX TZ string), else Kyiv. */
#define TIME_DB_KEY_TZ "time.tz"
#define TIME_TZ_DEFAULT "EET-2EEST,M3.5.0/3,M10.5.0/4"

static void set_time_zone(void) {
  char tz[64];
  if (db_read(TIME_DB_KEY_TZ, tz, sizeof(tz)) != ESP_OK || !tz[0]) {
    strcpy(tz, TIME_TZ_DEFAULT);
  }
  setenv("TZ", tz, 1);
  tzset();
  ESP_LOGI(TAG, "time zone %s", tz);
}

#define WIFI_CONNECT_MS 15000

/* Follows s_wifi_want (set by F+БП): while wanted and not connected, join the
 * network the WIFI program saved (reading it each time, so it can change);
 * while not wanted, stop Wi-Fi and free its buffers. Started lazily on the
 * first F+БП and then runs for good, polling every POLL_MS so the toggle is
 * prompt. A connection made by a program is left alone while wanted. */
#define WIFI_POLL_MS 500

static void wifi_task(void *arg) {
  for (;;) {
    if (s_wifi_want) {
      char ssid[NET_SSID_MAX + 1], pass[NET_PASS_MAX + 1];
      if (!net_wifi_status(NULL, NULL) && db_read(NET_DB_KEY_SSID, ssid, sizeof(ssid)) == ESP_OK && ssid[0]) {
        if (db_read(NET_DB_KEY_PASS, pass, sizeof(pass)) != ESP_OK) {
          pass[0] = '\0';
        }
        esp_err_t err = net_wifi_connect(ssid, pass, WIFI_CONNECT_MS);
        if (err != ESP_OK) {
          ESP_LOGW(TAG, "Wi-Fi %s: %s", ssid, esp_err_to_name(err));
        }
      }
    } else {
      net_wifi_down(); /* a no-op when already down */
    }
    vTaskDelay(pdMS_TO_TICKS(WIFI_POLL_MS));
  }
}

/* F+БП handler (called from the monitor task): flip the want flag, starting
 * the Wi-Fi task on the first enable. Returns the new state for the notice. */
static bool wifi_toggle(void) {
  static bool task_started;
  s_wifi_want = !s_wifi_want;
  if (s_wifi_want && !task_started) {
    task_started = true;
    xTaskCreatePinnedToCore(wifi_task, "wifi_join", 4096, NULL, 2, NULL, 0);
  }
  return s_wifi_want;
}

/* A .py file from the navigator runs as UPYTHON.ELF <file>. */
#define PYTHON_PATH EXEC_BIN_DIR "/UPYTHON.ELF"

static bool is_script(const char *path) {
  const char *dot = strrchr(path, '.');
  return dot && strcasecmp(dot, ".py") == 0;
}

/* A .c file from the navigator compiles as CC.ELF <file>. */
#define CC_PATH EXEC_BIN_DIR "/CC.ELF"

static bool is_source(const char *path) {
  const char *dot = strrchr(path, '.');
  return dot && strcasecmp(dot, ".c") == 0;
}

/* A sound file from the navigator plays as AUDIOPLAYER.ELF <file>. */
#define PLAYER_PATH EXEC_BIN_DIR "/AUDIOPLAYER.ELF"

static bool is_audio(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot) {
    return false;
  }
  return strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".mp3") == 0 ||
         strcasecmp(dot, ".ogg") == 0 || strcasecmp(dot, ".flac") == 0;
}

/* An image file from the navigator opens in IMAGEVIEWER.ELF <file>. */
#define IMAGEVIEWER_PATH EXEC_BIN_DIR "/IMAGEVIEWER.ELF"

static bool is_image(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot) {
    return false;
  }
  return strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".bmp") == 0 ||
         strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 ||
         strcasecmp(dot, ".gif") == 0;
}

/* A text file from the navigator opens in TEXTVIEWER.ELF <file>, which is
 * built from apps/textviewer. */
#define TEXTVIEWER_PATH EXEC_BIN_DIR "/TEXTVIEWER.ELF"

static bool is_text(const char *path) {
  static const char *const exts[] = {".txt", ".md", ".log", ".ini", ".cfg", ".csv", ".json", ".h"};
  const char *dot = strrchr(path, '.');
  if (!dot) {
    return false;
  }
  for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
    if (strcasecmp(dot, exts[i]) == 0) {
      return true;
    }
  }
  return false;
}

/* The monitor is a console of its own on the same display. */
static void start_monitor(macjr_tty_t *tty, macjr_hal_display_t *display) {
  macjr_tty_t *own = macjr_tty_create(display, 8, 16);
  if (!own || monitor_start(tty, own) != ESP_OK) {
    ESP_LOGW(TAG, "monitor_start failed");
    macjr_tty_destroy(own);
  }
}

void app_main(void) {
  if (sdcard_init() != ESP_OK) {
    ESP_LOGW(TAG, "sdcard_init failed, continuing without SD card");
  } else if (db_init() != ESP_OK) {
    ESP_LOGW(TAG, "db_init failed, continuing without config");
  }
  if (log_init() != ESP_OK) { /* after the db: the saved records come back */
    ESP_LOGW(TAG, "log_init failed, continuing without /dev/log");
  }
  if (ttydev_init() != ESP_OK) { /* at boot, so /dev lists it before any program runs */
    ESP_LOGW(TAG, "ttydev_init failed: programs print to the serial console");
  }

  set_time_zone();
  /* Wi-Fi is not started here: F+БП brings it up on demand (see wifi_start). */

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

  macjr_tty_set_front(tty);
  start_monitor(tty, display);
  monitor_set_wifi_cb(wifi_toggle); /* F+БП toggles Wi-Fi, with a notice */

  for (;;) {
    char path[64];
    const boot_builtin_t *builtin;
    boot_select(tty, NULL, 0, path, sizeof(path), &builtin); /* no built-in programs now */
    if (builtin) {
      exec_launch_main(tty, builtin->name, builtin->main);
    } else if (is_script(path)) {
      exec_launch_with(tty, PYTHON_PATH, path);
    } else if (is_source(path)) {
      exec_launch_with(tty, CC_PATH, path);
    } else if (is_audio(path)) {
      exec_launch_with(tty, PLAYER_PATH, path);
    } else if (is_image(path)) {
      exec_launch_with(tty, IMAGEVIEWER_PATH, path);
    } else if (is_text(path)) {
      exec_launch_with(tty, TEXTVIEWER_PATH, path);
    } else {
      exec_launch(tty, path);
    }
  }
}
