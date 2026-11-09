/*
 * USB OTG port bring-up: PHY on the internal OTG controller, TinyUSB stack
 * and the task that runs it, as a device (usbdev_start: the host sees
 * g_usbdev_mode, usb_descriptors.c) or as a host (usb_kbd.c: a USB
 * keyboard). One role at a time; each stops only its own.
 */
#include "usbdev_priv.h"

#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

#define TUSB_TASK_STACK 4096
#define TUSB_TASK_PRIO 5
#define TUSB_POLL_MS 50 /* how often the task checks for port_down() */

static const char *TAG = "usbdev";

usbdev_mode_t g_usbdev_mode;

static usb_phy_handle_t s_phy;
static tusb_role_t s_role = TUSB_ROLE_INVALID; /* TUSB_ROLE_INVALID: port down */
static volatile TaskHandle_t s_stopper;      /* port_down() waiting for the task */

static void tusb_task(void *arg) {
  bool host = (bool)(uintptr_t)arg;
  while (!s_stopper) {
    if (host) {
      tuh_task_ext(TUSB_POLL_MS, false); /* blocks until there is USB work */
    } else {
      tud_task_ext(TUSB_POLL_MS, false);
    }
  }
  xTaskNotifyGive(s_stopper);
  vTaskDelete(NULL);
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

esp_err_t usb_port_up(tusb_role_t role) {
  if (s_role != TUSB_ROLE_INVALID) {
    return ESP_ERR_INVALID_STATE;
  }
  bool host = role == TUSB_ROLE_HOST;
  usb_phy_config_t phy_conf = {
      .controller = USB_PHY_CTRL_OTG,
      .otg_mode = host ? USB_OTG_MODE_HOST : USB_OTG_MODE_DEVICE,
      .target = USB_PHY_TARGET_INT,
  };
  esp_err_t ret = usb_new_phy(&phy_conf, &s_phy);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "usb_new_phy failed: %s", esp_err_to_name(ret));
    return ret;
  }
  const tusb_rhport_init_t init = {.role = role, .speed = TUSB_SPEED_FULL};
  if (!tusb_rhport_init(0, &init)) {
    ESP_LOGE(TAG, "TinyUSB init failed");
    usb_del_phy(s_phy);
    phy_to_serial_jtag();
    return ESP_FAIL;
  }
  if (xTaskCreate(tusb_task, "TinyUSB", TUSB_TASK_STACK, (void *)(uintptr_t)host, TUSB_TASK_PRIO, NULL) != pdPASS) {
    tusb_deinit(0);
    usb_del_phy(s_phy);
    phy_to_serial_jtag();
    return ESP_ERR_NO_MEM;
  }
  s_role = role;
  return ESP_OK;
}

bool usb_port_role_is(tusb_role_t role) {
  return s_role == role;
}

void usb_port_down(void) {
  if (s_role == TUSB_ROLE_INVALID) {
    return;
  }
  s_stopper = xTaskGetCurrentTaskHandle();
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* the task is out of TinyUSB */
  s_stopper = NULL;

  tusb_deinit(0);
  usb_del_phy(s_phy);
  s_phy = NULL;
  phy_to_serial_jtag();
  s_role = TUSB_ROLE_INVALID;
  ESP_LOGI(TAG, "USB back to USB-Serial-JTAG");
}

esp_err_t usbdev_start(usbdev_mode_t mode) {
  g_usbdev_mode = mode; /* read only once the device is up */
  esp_err_t ret = usb_port_up(TUSB_ROLE_DEVICE);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "USB device up as %s", mode == USBDEV_MODE_MSC ? "SD card drive" : "UAC microphone");
  }
  return ret;
}

void usbdev_stop(void) {
  if (!usb_port_role_is(TUSB_ROLE_DEVICE)) {
    return; /* down, or a USB keyboard: not ours to stop */
  }
  tud_disconnect();
  usbdev_msc_set_card(NULL); /* waits for a transfer under way */
  usb_port_down();
}
