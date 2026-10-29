/*
 * USB descriptors for both device personalities. The UAC configuration is
 * the one usb_device_uac uses on its own (one mic, interfaces 0-1); the MSC
 * configuration is a single bulk-only mass-storage interface. Each has its
 * own PID so the host never mixes up cached descriptors.
 */
#include <string.h>

#include "tusb.h"
#include "uac_descriptors.h"
#include "usbdev_priv.h"

#define USB_VID 0x303A
#define USB_PID_UAC 0x8000
#define USB_PID_MSC 0x8001

enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL, STR_ITF, STR_ITF2, STR_COUNT };

/* ---- device ------------------------------------------------------------ */

static const tusb_desc_device_t s_device_uac = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* audio uses an Interface Association Descriptor */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID_UAC,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_t s_device_msc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00, /* class defined by the interface */
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID_MSC,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *)(g_usbdev_mode == USBDEV_MODE_MSC ? &s_device_msc : &s_device_uac);
}

/* ---- configuration ----------------------------------------------------- */

enum { ITF_UAC_CONTROL = 0, ITF_UAC_MIC, ITF_UAC_TOTAL };
_Static_assert(ITF_UAC_MIC == USBDEV_ITF_MIC, "USBDEV_ITF_MIC out of sync");

#define UAC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_AUDIO_DEVICE_DESC_LEN)
#define EP_AUDIO_OUT 0x01
#define EP_AUDIO_FB 0x81
#define EP_AUDIO_IN 0x82

static const uint8_t s_config_uac[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_UAC_TOTAL, 0, UAC_TOTAL_LEN, 0x00, 100),
    TUD_AUDIO_DESCRIPTOR(ITF_UAC_CONTROL, STR_ITF, EP_AUDIO_OUT, EP_AUDIO_IN, EP_AUDIO_FB),
};

#define MSC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
#define EP_MSC_OUT 0x01
#define EP_MSC_IN 0x81

static const uint8_t s_config_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, MSC_TOTAL_LEN, 0x00, 100),
    TUD_MSC_DESCRIPTOR(0, STR_ITF, EP_MSC_OUT, EP_MSC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return g_usbdev_mode == USBDEV_MODE_MSC ? s_config_msc : s_config_uac;
}

/* ---- strings ----------------------------------------------------------- */

static const char *const s_strings_uac[STR_COUNT] = {
    [STR_MANUFACTURER] = "MACJr", [STR_PRODUCT] = "MACJr Mic", [STR_SERIAL] = "12345678",
    [STR_ITF] = "usb uac",        [STR_ITF2] = "microphone",
};

static const char *const s_strings_msc[STR_COUNT] = {
    [STR_MANUFACTURER] = "MACJr", [STR_PRODUCT] = "MACJr SD card", [STR_SERIAL] = "12345678",
    [STR_ITF] = "SD card",
};

static uint16_t s_desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  uint8_t count;
  if (index == STR_LANG) {
    s_desc_str[1] = 0x0409; /* English */
    count = 1;
  } else {
    const char *const *table = g_usbdev_mode == USBDEV_MODE_MSC ? s_strings_msc : s_strings_uac;
    const char *str = index < STR_COUNT ? table[index] : NULL;
    if (!str) {
      return NULL;
    }
    count = (uint8_t)strlen(str);
    if (count > 31) {
      count = 31;
    }
    for (uint8_t i = 0; i < count; i++) {
      s_desc_str[1 + i] = (uint8_t)str[i];
    }
  }
  s_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
  return s_desc_str;
}
