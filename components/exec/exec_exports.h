#ifndef MACJR_EXEC_EXPORTS_H
#define MACJR_EXEC_EXPORTS_H

#include "esp_elf.h"

/* Firmware functions programs may call (exports.c). */
extern const struct esp_elfsym g_exec_exports[];

#endif
