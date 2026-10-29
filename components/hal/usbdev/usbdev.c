/*
 * USB device bring-up: PHY on the internal OTG controller, TinyUSB stack and
 * the task that runs it. Which device the host sees depends on
 * g_usbdev_mode (usb_descriptors.c).
 */
#include "usbdev_priv.h"

#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

#define TUSB_TASK_STACK 4096
#define TUSB_TASK_PRIO 5

static const char *TAG = "usbdev";

usbdev_mode_t g_usbdev_mode;

static usb_phy_handle_t s_phy;
static bool s_started;

static void tusb_task(void *arg) {
  (void)arg;
  for (;;) {
    tud_task(); /* blocks until there is USB work */
  }
}

esp_err_t usbdev_start(usbdev_mode_t mode) {
  if (s_started) {
    return ESP_ERR_INVALID_STATE;
  }
  g_usbdev_mode = mode;

  usb_phy_config_t phy_conf = {
      .controller = USB_PHY_CTRL_OTG,
      .otg_mode = USB_OTG_MODE_DEVICE,
      .target = USB_PHY_TARGET_INT,
  };
  esp_err_t ret = usb_new_phy(&phy_conf, &s_phy);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "usb_new_phy failed: %s", esp_err_to_name(ret));
    return ret;
  }
  if (!tusb_init()) {
    ESP_LOGE(TAG, "TinyUSB init failed");
    return ESP_FAIL;
  }
  if (xTaskCreate(tusb_task, "TinyUSB", TUSB_TASK_STACK, NULL, TUSB_TASK_PRIO, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  s_started = true;
  ESP_LOGI(TAG, "USB device up as %s", mode == USBDEV_MODE_MSC ? "SD card drive" : "UAC microphone");
  return ESP_OK;
}
