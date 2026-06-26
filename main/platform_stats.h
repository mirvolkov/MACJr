/*
 * The two "live" numbers shown in the Turbo C demo's Linking dialog --
 * flash size and free memory -- mean different things on a real ESP32-S3
 * board vs. a desktop host, so the query itself is isolated per platform
 * (implemented directly in main_esp32.c / main_macos.c) behind this one
 * header.
 */
#ifndef MACJR_PLATFORM_STATS_H
#define MACJR_PLATFORM_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32: real SPI flash chip size. Host: free space on the root volume,
 * the closest desktop analog to "persistent storage size". */
uint32_t macjr_platform_flash_kb(void);

/* ESP32: real free heap. Host: this process's resident memory, since a
 * desktop OS doesn't expose a single "free heap" the way FreeRTOS does. */
uint32_t macjr_platform_heap_free_kb(void);

#ifdef __cplusplus
}
#endif

#endif
