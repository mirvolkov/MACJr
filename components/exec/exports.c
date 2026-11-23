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
#include <time.h>
#include <unistd.h>

#include "db.h"
#include "logdev.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "private/elf_symbol.h"

/* libgcc's 64-bit helpers (no header declares them) */
extern long long __divdi3(long long, long long);
extern long long __moddi3(long long, long long);
extern unsigned long long __udivdi3(unsigned long long, unsigned long long);
extern unsigned long long __umoddi3(unsigned long long, unsigned long long);
extern long long __ashldi3(long long, int);
extern long long __ashrdi3(long long, int);
extern long long __lshrdi3(long long, int);
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

/* built-in font (components/hal/display/tty/src/font_builtin.c) */
extern const unsigned char macjr_font_builtin_8x16[256][16];
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
  ESP_ELFSYM_EXPORT(macjr_font_builtin_8x16),
  ESP_ELFSYM_EXPORT(macjr_tty_display),
  ESP_ELFSYM_EXPORT(macjr_display_flush),
  ESP_ELFSYM_EXPORT(macjr_tty_list_init),
  ESP_ELFSYM_EXPORT(macjr_tty_list_set_count),
  ESP_ELFSYM_EXPORT(macjr_tty_list_move),
  ESP_ELFSYM_EXPORT(macjr_tty_list_draw),
  ESP_ELFSYM_EXPORT(macjr_tty_list_scroll),
  ESP_ELFSYM_EXPORT(macjr_tty_list_draw_view),
  ESP_ELFSYM_EXPORT(macjr_tty_window_center),
  ESP_ELFSYM_EXPORT(macjr_tty_window_draw),
  ESP_ELFSYM_EXPORT(macjr_tty_window_text),

  /* system monitor */
  ESP_ELFSYM_EXPORT(monitor_is_open),
  ESP_ELFSYM_EXPORT(monitor_set_enabled),

  /* board HALs */
  ESP_ELFSYM_EXPORT(db_create),
  ESP_ELFSYM_EXPORT(db_read),
  ESP_ELFSYM_EXPORT(db_update),
  ESP_ELFSYM_EXPORT(db_listen),

  /* the system log (/dev/log: open, read, write, select through the
   * calls below; log_open() and log_read() are inline) */
  ESP_ELFSYM_EXPORT(log_write),
  ESP_ELFSYM_EXPORT(log_vwrite),
  ESP_ELFSYM_EXPORT(log_level_char),
  ESP_ELFSYM_EXPORT(speaker_write),
  ESP_ELFSYM_EXPORT(speaker_set_sample_rate),
  ESP_ELFSYM_EXPORT(speaker_sample_rate),
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
  ESP_ELFSYM_EXPORT(sdr_gen_init),
  ESP_ELFSYM_EXPORT(sdr_gen_set_freq),
  ESP_ELFSYM_EXPORT(sdr_gen_enable),
  EXEC_EXPORT_AS(sdcard_open_raw, exec_sdcard_open_raw),
  ESP_ELFSYM_EXPORT(usbdev_start),
  ESP_ELFSYM_EXPORT(usbdev_msc_set_card),
  ESP_ELFSYM_EXPORT(usbdev_msc_status),
  ESP_ELFSYM_EXPORT(png_decode_file),
  ESP_ELFSYM_EXPORT(jpeg_decode_file),

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
  EXEC_EXPORT_AS(write, exec_write),
  ESP_ELFSYM_EXPORT(mkdir),
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

  /* libgcc: 64-bit arithmetic for programs built on the board by CC.ELF
   * (programs built by GCC link their own copy) */
  ESP_ELFSYM_EXPORT(__divdi3),
  ESP_ELFSYM_EXPORT(__moddi3),
  ESP_ELFSYM_EXPORT(__udivdi3),
  ESP_ELFSYM_EXPORT(__umoddi3),
  ESP_ELFSYM_EXPORT(__ashldi3),
  ESP_ELFSYM_EXPORT(__ashrdi3),
  ESP_ELFSYM_EXPORT(__lshrdi3),

  /* for CC.ELF: its memory in PSRAM, the C library TinyCC uses, and the
   * firmware's symbol table to check a program's imports against */
  ESP_ELFSYM_EXPORT(heap_caps_realloc),
  ESP_ELFSYM_EXPORT(heap_caps_free),
  ESP_ELFSYM_EXPORT(elf_find_sym),
  ESP_ELFSYM_EXPORT(esp_pthread_get_default_config),
  ESP_ELFSYM_EXPORT(esp_pthread_set_cfg),
  ESP_ELFSYM_EXPORT(lseek),
  ESP_ELFSYM_EXPORT(qsort),
  ESP_ELFSYM_EXPORT(sprintf),
  ESP_ELFSYM_EXPORT(strstr),
  ESP_ELFSYM_EXPORT(strcat),
  ESP_ELFSYM_EXPORT(strtoull),
  ESP_ELFSYM_EXPORT(strtof),
  ESP_ELFSYM_EXPORT(strtold),
  ESP_ELFSYM_EXPORT(ldexpl),
  ESP_ELFSYM_EXPORT(time),
  ESP_ELFSYM_EXPORT(localtime),

  /* stdio on the program's tty (/dev/tty), not the serial console */
  EXEC_EXPORT_AS(printf, exec_printf),
  EXEC_EXPORT_AS(vprintf, exec_vprintf),
  EXEC_EXPORT_AS(puts, exec_puts),
  EXEC_EXPORT_AS(putchar, exec_putchar),

  /* C library missing from the loader's table */
  ESP_ELFSYM_EXPORT(snprintf),
  ESP_ELFSYM_EXPORT(vsnprintf),
  ESP_ELFSYM_EXPORT(atoi),
  ESP_ELFSYM_EXPORT(strtoul),
  ESP_ELFSYM_EXPORT(strcpy),
  ESP_ELFSYM_EXPORT(memmove),
  ESP_ELFSYM_EXPORT(memcmp),
  ESP_ELFSYM_EXPORT(strncmp),
  ESP_ELFSYM_EXPORT(strncpy),

  /* math — basic set (the MK-52) plus the full C99 libm needed by MicroPython */
  ESP_ELFSYM_EXPORT(sin),
  ESP_ELFSYM_EXPORT(cos),
  ESP_ELFSYM_EXPORT(tan),
  ESP_ELFSYM_EXPORT(asin),
  ESP_ELFSYM_EXPORT(acos),
  ESP_ELFSYM_EXPORT(atan),
  ESP_ELFSYM_EXPORT(atan2),
  ESP_ELFSYM_EXPORT(sinh),
  ESP_ELFSYM_EXPORT(cosh),
  ESP_ELFSYM_EXPORT(tanh),
  ESP_ELFSYM_EXPORT(asinh),
  ESP_ELFSYM_EXPORT(acosh),
  ESP_ELFSYM_EXPORT(atanh),
  ESP_ELFSYM_EXPORT(exp),
  ESP_ELFSYM_EXPORT(exp2),
  ESP_ELFSYM_EXPORT(expm1),
  ESP_ELFSYM_EXPORT(log),
  ESP_ELFSYM_EXPORT(log2),
  ESP_ELFSYM_EXPORT(log10),
  ESP_ELFSYM_EXPORT(log1p),
  ESP_ELFSYM_EXPORT(pow),
  ESP_ELFSYM_EXPORT(sqrt),
  ESP_ELFSYM_EXPORT(cbrt),
  ESP_ELFSYM_EXPORT(fabs),
  ESP_ELFSYM_EXPORT(ceil),
  ESP_ELFSYM_EXPORT(floor),
  ESP_ELFSYM_EXPORT(trunc),
  ESP_ELFSYM_EXPORT(round),
  ESP_ELFSYM_EXPORT(nearbyint),
  ESP_ELFSYM_EXPORT(fmod),
  ESP_ELFSYM_EXPORT(modf),
  ESP_ELFSYM_EXPORT(frexp),
  ESP_ELFSYM_EXPORT(ldexp),
  ESP_ELFSYM_EXPORT(copysign),
  ESP_ELFSYM_EXPORT(hypot),

  ESP_ELFSYM_END,
};
