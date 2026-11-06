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
#define TUSB_POLL_MS 50 /* how often the task checks for usbdev_stop() */

static const char *TAG = "usbdev";

usbdev_mode_t g_usbdev_mode;

static usb_phy_handle_t s_phy;
static bool s_started;
static volatile TaskHandle_t s_stopper; /* usbdev_stop() waiting for the task */

static void tusb_task(void *arg) {
  (void)arg;
  while (!s_stopper) {
    tud_task_ext(TUSB_POLL_MS, false); /* blocks until there is USB work */
  }
  xTaskNotifyGive(s_stopper);
  vTaskDelete(NULL);
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

/* usb_del_phy() leaves the internal PHY routed to USB OTG; creating a PHY
 * for USB-Serial-JTAG is what routes it back. */
static void phy_to_serial_jtag(void) {
  usb_phy_config_t phy_conf = {
      .controller = USB_PHY_CTRL_SERIAL_JTAG,
      .otg_mode = USB_PHY_MODE_DEFAULT,
      .target = USB_PHY_TARGET_INT,
  };
  usb_phy_handle_t phy;
  esp_err_t ret = usb_new_phy(&phy_conf, &phy);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "USB-Serial-JTAG not restored: %s", esp_err_to_name(ret));
    return;
  }
  usb_del_phy(phy);
}

void usbdev_stop(void) {
  if (!s_started) {
    return;
  }
  tud_disconnect();
  usbdev_msc_set_card(NULL); /* waits for a transfer under way */

  s_stopper = xTaskGetCurrentTaskHandle();
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* the task is out of TinyUSB */
  s_stopper = NULL;

  tusb_deinit(0);
  usb_del_phy(s_phy);
  s_phy = NULL;
  phy_to_serial_jtag();
  s_started = false;
  ESP_LOGI(TAG, "USB back to USB-Serial-JTAG");
}
