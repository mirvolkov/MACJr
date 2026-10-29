#pragma once

#include "usbdev.h"

/* Mode chosen by usbdev_start(); usb_descriptors.c reports the matching
 * device / configuration descriptors. */
extern usbdev_mode_t g_usbdev_mode;
