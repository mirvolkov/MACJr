/* ESP32 implementation of platform_stats.h -- real flash chip size and
 * real FreeRTOS free heap. */
#include "platform_stats.h"

#include "esp_flash.h"
#include "esp_system.h"

uint32_t macjr_platform_flash_kb(void) {
  uint32_t flash_bytes = 0;
  esp_flash_get_size(NULL, &flash_bytes);
  return flash_bytes / 1024;
}

uint32_t macjr_platform_heap_free_kb(void) {
  return esp_get_free_heap_size() / 1024;
}
