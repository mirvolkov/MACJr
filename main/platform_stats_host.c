/* Host (macOS) implementation of platform_stats.h. There's no SPI flash
 * chip or FreeRTOS heap on a desktop, so these stand in with the closest
 * real numbers the OS exposes: free space on the root volume, and this
 * process's own resident memory. */
#include "platform_stats.h"

#include <sys/resource.h>
#include <sys/statvfs.h>

uint32_t macjr_platform_flash_kb(void) {
  struct statvfs st;
  if (statvfs("/", &st) != 0) {
    return 0;
  }
  return (uint32_t)(((uint64_t)st.f_bavail * (uint64_t)st.f_frsize) / 1024);
}

uint32_t macjr_platform_heap_free_kb(void) {
  struct rusage ru;
  if (getrusage(RUSAGE_SELF, &ru) != 0) {
    return 0;
  }
  /* macOS reports ru_maxrss in bytes (Linux reports KB -- this project only
   * targets macOS for the host build, so no #ifdef needed here). */
  return (uint32_t)(ru.ru_maxrss / 1024);
}
