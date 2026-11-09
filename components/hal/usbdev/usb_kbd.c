/*
 * USB keyboard: the OTG port as a TinyUSB host with the HID class. A
 * keyboard plugged in is asked for the boot protocol, whose 8-byte report
 * (modifiers, reserved, six key codes) is the same for every keyboard. A
 * key that appears in a report and wasn't in the one before was pressed;
 * it goes to /dev/kbrd through keyboard_inject():
 *
 *   printable keys, Shift   ASCII 32-126 (US layout); Tab 9
 *   Enter                   В↑ (KEY_MK_ENTER)
 *   Esc, Backspace          Cx (KEY_ESC)
 *   ↑ / ↓                   П→x / x→П, the keys lists move with
 *
 * No auto-repeat: holding a key types it once. No Caps Lock.
 */
#include "usbdev_priv.h"

#include <string.h>

#include "esp_log.h"
#include "keyboard.h"
#include "tusb.h"

static const char *TAG = "usb_kbd";

static const uint8_t s_ascii[128][2] = {HID_KEYCODE_TO_ASCII};

static volatile bool s_connected;
static uint8_t s_prev[6]; /* key codes of the last report */

esp_err_t usb_kbd_start(void) {
  memset(s_prev, 0, sizeof(s_prev));
  s_connected = false;
  esp_err_t ret = usb_port_up(TUSB_ROLE_HOST);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "USB host up: plug in a keyboard");
  }
  return ret;
}

void usb_kbd_stop(void) {
  if (usb_port_role_is(TUSB_ROLE_HOST)) {
    usb_port_down();
    s_connected = false;
  }
}

bool usb_kbd_active(void) {
  return usb_port_role_is(TUSB_ROLE_HOST);
}

bool usb_kbd_connected(void) {
  return s_connected;
}

/* The /dev/kbrd code for HID key `kc`, or 0 for none. */
static uint8_t translate(uint8_t kc, bool shift) {
  switch (kc) {
    case HID_KEY_ENTER:
    case HID_KEY_KEYPAD_ENTER:
      return KEY_MK_ENTER;
    case HID_KEY_ESCAPE:
    case HID_KEY_BACKSPACE:
      return KEY_ESC;
    case HID_KEY_ARROW_UP:
      return KEY_MK_P_TO_X;
    case HID_KEY_ARROW_DOWN:
      return KEY_MK_X_TO_P;
    default:
      break;
  }
  if (kc >= 128) {
    return 0;
  }
  uint8_t ch = s_ascii[kc][shift ? 1 : 0];
  return ch == '\t' || (ch >= 0x20 && ch < 0x7F) ? ch : 0;
}

static bool was_down(uint8_t kc) {
  for (int i = 0; i < 6; i++) {
    if (s_prev[i] == kc) {
      return true;
    }
  }
  return false;
}

static void keyboard_report(const hid_keyboard_report_t *rep) {
  bool shift = rep->modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);
  bool ctrl_alt = rep->modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL |
                                   KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT |
                                   KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI);
  for (int i = 0; i < 6; i++) {
    uint8_t kc = rep->keycode[i];
    if (kc && !was_down(kc) && !ctrl_alt) {
      uint8_t code = translate(kc, shift);
      if (code) {
        keyboard_inject(code);
      }
    }
  }
  memcpy(s_prev, rep->keycode, sizeof(s_prev));
}

/* ---- TinyUSB host callbacks (the TinyUSB task) ---- */

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *report_desc, uint16_t desc_len) {
  (void)report_desc;
  (void)desc_len;
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(dev_addr, &vid, &pid);
  if (tuh_hid_interface_protocol(dev_addr, idx) != HID_ITF_PROTOCOL_KEYBOARD) {
    ESP_LOGI(TAG, "HID %04x:%04x interface %u is not a keyboard", vid, pid, idx);
    return;
  }
  ESP_LOGI(TAG, "keyboard %04x:%04x connected", vid, pid);
  memset(s_prev, 0, sizeof(s_prev));
  s_connected = true;
  tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx) {
  (void)dev_addr;
  (void)idx;
  ESP_LOGI(TAG, "keyboard disconnected");
  s_connected = false;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, uint8_t const *report, uint16_t len) {
  if (tuh_hid_interface_protocol(dev_addr, idx) == HID_ITF_PROTOCOL_KEYBOARD &&
      len >= sizeof(hid_keyboard_report_t)) {
    keyboard_report((const hid_keyboard_report_t *)report);
  }
  tuh_hid_receive_report(dev_addr, idx); /* the next one */
}
