/*
 * USB OTG -> UAC 2.0 bridge for the mic HAL. The usbdev HAL brings up
 * TinyUSB in UAC mode; usb_device_uac (built with
 * CONFIG_USB_DEVICE_UAC_AS_PART) adds the audio class and a mic task that
 * asks for CONFIG_UAC_MIC_INTERVAL_MS worth of samples every interval; we
 * answer by blocking on mic_read(), so the ADC clock paces
 * the stream. Channel count / sample rate / 16-bit format come from
 * sdkconfig and must match MIC_CHANNELS / MIC_SAMPLE_RATE.
 */
#include "mic.h"

#include <string.h>

#include "esp_log.h"
#include "usb_device_uac.h"
#include "usbdev.h"

#if CONFIG_UAC_MIC_CHANNEL_NUM != MIC_CHANNELS
#error "CONFIG_UAC_MIC_CHANNEL_NUM must equal MIC_CHANNELS"
#endif
#if CONFIG_UAC_SAMPLE_RATE != MIC_SAMPLE_RATE
#error "CONFIG_UAC_SAMPLE_RATE must equal MIC_SAMPLE_RATE"
#endif
#if CONFIG_UAC_BYTES_PER_SAMPLE != 2
#error "mic HAL produces 16-bit samples; set CONFIG_UAC_BIT_RESOLUTION_16"
#endif

#define READ_TIMEOUT_MS 50

static const char *TAG = "mic_uac";

static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *ctx) {
  (void)ctx;
  const size_t frame_bytes = MIC_CHANNELS * sizeof(int16_t);
  size_t frame_count = len / frame_bytes;
  size_t got = 0;

  esp_err_t ret = mic_read((int16_t *)buf, frame_count, READ_TIMEOUT_MS, &got);
  if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
    ESP_LOGW(TAG, "mic_read failed: %s", esp_err_to_name(ret));
  }
  /* Pad short reads with silence so the host sees a steady stream. */
  memset(buf + got * frame_bytes, 0, len - got * frame_bytes);
  *bytes_read = len;
  return ESP_OK;
}

esp_err_t mic_uac_start(void) {
  esp_err_t ret = usbdev_start(USBDEV_MODE_UAC);
  if (ret != ESP_OK) {
    return ret;
  }
  uac_device_config_t cfg = {
      .skip_tinyusb_init = true, /* usbdev already runs TinyUSB */
      .mic_itf_num = USBDEV_ITF_MIC,
      .input_cb = uac_input_cb,
  };
  ret = uac_device_init(&cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "uac_device_init failed: %s", esp_err_to_name(ret));
    return ret;
  }
  ESP_LOGI(TAG, "USB UAC mic streaming (%d ch @ %d Hz)", MIC_CHANNELS, MIC_SAMPLE_RATE);
  return ESP_OK;
}
