/*
 * TinyUSB configuration for MACJr: the UAC audio class (settings from
 * usb_device_uac's tusb_config_uac.h, driven by the CONFIG_UAC_* options)
 * plus the mass-storage class. Both are compiled in; usb_descriptors.c
 * shows the host only the one chosen at runtime.
 */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#include "sdkconfig.h"
#include "uac_config.h"
#include "uac_descriptors.h"
#include "tusb_config_uac.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CONFIG_USB_HS 0

#define CFG_TUSB_OS OPT_OS_FREERTOS
#define CFG_TUSB_OS_INC_PATH freertos/
#define CFG_TUSB_DEBUG 0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))

#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64

/* Mass storage: a multiple of the 512-byte SD sector, so every read10 /
 * write10 callback covers whole sectors. */
#define CFG_TUD_MSC 1
#define CFG_TUD_MSC_EP_BUFSIZE 4096

#ifdef __cplusplus
}
#endif

#endif
