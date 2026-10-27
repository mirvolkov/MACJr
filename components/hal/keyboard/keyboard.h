#ifndef MACJR_KEYBOARD_H
#define MACJR_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Character device that delivers key presses, one byte per key. Open it
 * read-only; each open() gets its own queue, so every reader sees every
 * key. read() blocks until a key arrives unless O_NONBLOCK is set (then it
 * fails with EAGAIN). Key releases are not reported. */
#define KBD_DEV_PATH "/dev/kbrd"

/* Raw key codes, not yet mapped to ASCII: the 32 keys are numbered
 * KBD_KEYCODE_BASE..KBD_KEYCODE_BASE + KBD_KEY_COUNT - 1 row by row
 * (row pins 1, 12, 13, 14; columns by pin 2..11), so row pin 1 x col pin 5
 * is 128 and row pin 14 x col pin 11 is 159. */
#define KBD_KEYCODE_BASE 128
#define KBD_KEY_COUNT    32

/* Bring up the I2C bus and the MCP23018 expander, register KBD_DEV_PATH and
 * start the matrix scan task. Call once at startup. */
esp_err_t keyboard_init(void);

/* True if the key with this code is currently held down (debounced). Right
 * after keyboard_init() it waits for the first scans to finish, so it can be
 * used at boot to check for a key held since power-up. */
bool keyboard_key_down(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif
