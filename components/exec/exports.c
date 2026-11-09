/*
 * The firmware functions a program loaded from the SD card may call,
 * bound by name when the program is loaded. The loader's own libc table
 * (CONFIG_ELF_LOADER_LIBC_SYMBOLS: malloc, free, memcpy, memset, strlen,
 * strchr, strtod, printf, ...) comes on top of these.
 *
 * A program that needs something not listed here fails to load with
 * "Can't find symbol"; add the function to the right group below.
 * EXEC_EXPORT_AS() entries stand in for libc/FreeRTOS calls of the same
 * name (see exec_exports.h).
 */
#include "exec_exports.h"

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ir_rx.h"
#include "ir_tx.h"
#include "macjr.h"
#include "mic.h"
#include "monitor.h"
#include "net.h"
#include "png.h"
#include "sdcard.h"
#include "sdr.h"
#include "speaker.h"
#include "tty/list.h"
#include "tty/window.h"
#include "usbdev.h"

const struct esp_elfsym g_exec_exports[] = {
  /* MacJr API (sdk/include/macjr.h) */
  ESP_ELFSYM_EXPORT(sys_getkey),
  ESP_ELFSYM_EXPORT(sys_millis),
  ESP_ELFSYM_EXPORT(sys_delay),
  ESP_ELFSYM_EXPORT(sys_cpu),
  ESP_ELFSYM_EXPORT(sys_exit),

  /* tty (tty/tty.h) */
  ESP_ELFSYM_EXPORT(macjr_tty_write),
  ESP_ELFSYM_EXPORT(macjr_tty_header),
  ESP_ELFSYM_EXPORT(macjr_tty_winsize),
  ESP_ELFSYM_EXPORT(macjr_tty_display),
  ESP_ELFSYM_EXPORT(macjr_display_flush),
  ESP_ELFSYM_EXPORT(macjr_tty_list_init),
  ESP_ELFSYM_EXPORT(macjr_tty_list_set_count),
  ESP_ELFSYM_EXPORT(macjr_tty_list_move),
  ESP_ELFSYM_EXPORT(macjr_tty_list_draw),
  ESP_ELFSYM_EXPORT(macjr_tty_window_center),
  ESP_ELFSYM_EXPORT(macjr_tty_window_draw),
  ESP_ELFSYM_EXPORT(macjr_tty_window_text),

  /* system monitor */
  ESP_ELFSYM_EXPORT(monitor_is_open),
  ESP_ELFSYM_EXPORT(monitor_set_enabled),
  ESP_ELFSYM_EXPORT(monitor_set_redraw_cb),

  /* board HALs */
  ESP_ELFSYM_EXPORT(db_create),
  ESP_ELFSYM_EXPORT(db_read),
  ESP_ELFSYM_EXPORT(db_update),
  ESP_ELFSYM_EXPORT(db_watch),
  ESP_ELFSYM_EXPORT(speaker_channel_name),
  ESP_ELFSYM_EXPORT(speaker_set_volume),
  ESP_ELFSYM_EXPORT(speaker_volume),
  ESP_ELFSYM_EXPORT(speaker_set_mute),
  ESP_ELFSYM_EXPORT(speaker_muted),
  ESP_ELFSYM_EXPORT(speaker_save_volume),
  ESP_ELFSYM_EXPORT(ir_rx_read),
  ESP_ELFSYM_EXPORT(ir_rx_last_burst),
  ESP_ELFSYM_EXPORT(ir_proto_name),
  ESP_ELFSYM_EXPORT(ir_tx_send),
  ESP_ELFSYM_EXPORT(ir_tx_send_raw),
  ESP_ELFSYM_EXPORT(net_wifi_scan),
  ESP_ELFSYM_EXPORT(net_wifi_connect),
  ESP_ELFSYM_EXPORT(net_wifi_disconnect),
  ESP_ELFSYM_EXPORT(net_wifi_status),
  ESP_ELFSYM_EXPORT(mic_init),
  ESP_ELFSYM_EXPORT(mic_uac_start),
  ESP_ELFSYM_EXPORT(sdr_init),
  ESP_ELFSYM_EXPORT(sdr_set_freq),
  EXEC_EXPORT_AS(sdcard_open_raw, exec_sdcard_open_raw),
  ESP_ELFSYM_EXPORT(usbdev_start),
  ESP_ELFSYM_EXPORT(usbdev_msc_set_card),
  ESP_ELFSYM_EXPORT(usbdev_msc_status),
  ESP_ELFSYM_EXPORT(png_decode_file),

  /* ESP-IDF / FreeRTOS */
  ESP_ELFSYM_EXPORT(esp_log),
  ESP_ELFSYM_EXPORT(esp_log_timestamp),
  ESP_ELFSYM_EXPORT(esp_err_to_name),
  ESP_ELFSYM_EXPORT(esp_timer_get_time),
  EXEC_EXPORT_AS(vTaskDelay, exec_vTaskDelay),
  ESP_ELFSYM_EXPORT(esp_flash_get_size),
  ESP_ELFSYM_EXPORT(esp_get_free_heap_size),

  /* POSIX files (VFS) */
  EXEC_EXPORT_AS(open, exec_open),
  EXEC_EXPORT_AS(close, exec_close),
  EXEC_EXPORT_AS(read, exec_read),
  ESP_ELFSYM_EXPORT(fcntl),
  ESP_ELFSYM_EXPORT(stat),
  ESP_ELFSYM_EXPORT(opendir),
  ESP_ELFSYM_EXPORT(readdir),
  ESP_ELFSYM_EXPORT(closedir),
  EXEC_EXPORT_AS(select, exec_select),

  /* process control */
  EXEC_EXPORT_AS(usleep, exec_usleep),
  EXEC_EXPORT_AS(sleep, exec_sleep),
  EXEC_EXPORT_AS(exit, exec_exit),

  /* C library missing from the loader's table */
  ESP_ELFSYM_EXPORT(snprintf),
  ESP_ELFSYM_EXPORT(vsnprintf),
  ESP_ELFSYM_EXPORT(atoi),
  ESP_ELFSYM_EXPORT(strtoul),
  ESP_ELFSYM_EXPORT(strcpy),
  ESP_ELFSYM_EXPORT(memmove),

  /* math (the MK-52) */
  ESP_ELFSYM_EXPORT(sin),
  ESP_ELFSYM_EXPORT(cos),
  ESP_ELFSYM_EXPORT(asin),
  ESP_ELFSYM_EXPORT(acos),
  ESP_ELFSYM_EXPORT(atan),
  ESP_ELFSYM_EXPORT(exp),
  ESP_ELFSYM_EXPORT(log),
  ESP_ELFSYM_EXPORT(log10),
  ESP_ELFSYM_EXPORT(pow),
  ESP_ELFSYM_EXPORT(sqrt),
  ESP_ELFSYM_EXPORT(floor),
  ESP_ELFSYM_EXPORT(trunc),

  ESP_ELFSYM_END,
};
