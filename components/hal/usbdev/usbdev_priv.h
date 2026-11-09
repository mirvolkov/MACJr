#pragma once

#include "usbdev.h"
#include "tusb.h"

/* Mode chosen by usbdev_start(); usb_descriptors.c reports the matching
 * device / configuration descriptors. */
extern usbdev_mode_t g_usbdev_mode;

/* The OTG port in TinyUSB `role` (device or host), with its task.
 * ESP_ERR_INVALID_STATE if it is up already, in either role. */
esp_err_t usb_port_up(tusb_role_t role);

/* Stop the task and TinyUSB, give the port back to USB-Serial-JTAG. */
void usb_port_down(void);

bool usb_port_role_is(tusb_role_t role);
