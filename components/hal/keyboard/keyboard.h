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
 * fails with EAGAIN). select() reports the descriptor readable while keys
 * are waiting, so a program can wait for keys together with other
 * descriptors, or with a timeout. Key releases are not reported.
 *
 * Two exceptions to "one byte per key, on press":
 *  - Cx is sent as KEY_ESC (ASCII 27). Every program treats it as "back":
 *    leave the current screen, or end the program.
 *  - Keys that start a chord (F, K, Cx, ↑↓) are sent when released, unless
 *    a partner is pressed while they are held: then the chord's own code
 *    is sent instead of either key (KEY_MK_F_K, KEY_MK_F_ESC,
 *    KEY_MK_F_UPDOWN, KEY_MK_F_A_UP). Another key pressed while one is held sends the
 *    held key first. */
#define KBD_DEV_PATH "/dev/kbrd"

/* Raw key codes, not yet mapped to ASCII: the 32 keys are numbered
 * KBD_KEYCODE_BASE..KBD_KEYCODE_BASE + KBD_KEY_COUNT - 1 row by row
 * (row pins 1, 12, 13, 14; columns by pin 2..11), so row pin 1 x col pin 5
 * is 128 and row pin 14 x col pin 11 is 159. Cx (140) alone is sent as
 * KEY_ESC instead. */
#define KBD_KEYCODE_BASE 128
#define KBD_KEY_COUNT    32

/*
 * MK-52 keyboard: physical layout (as seen from the front) -> key code.
 * Mapped by pressing every key on the real board.
 *
 *          col 1    col 2    col 3    col 4    col 5    col 6    col 7    col 8
 *  row 1   F   158  ШГ→ 155  П→x 156  7   131  8   130  9   138  −   144  ÷   149
 *  row 2   K   150  ←ШГ 151  x→П 157  4   137  5   135  6   136  +   142  ×   143
 *  row 3   ↑↓  128  В/О 153  БП  154  1   134  2   133  3   139  ↔   147  В↑  148
 *  row 4   А↑  129  С/П 152  ПП  159  0   132  .   145  /-/ 146  ВП  141  Cx  140*
 *
 *  * sent as KEY_ESC (27)
 *
 * Sorted by code:
 *  128 ↑↓   129 А↑   130 8    131 7    132 0    133 2    134 1    135 5
 *  136 6    137 4    138 9    139 3    140 Cx   141 ВП   142 +    143 ×
 *  144 −    145 .    146 /-/  147 ↔    148 В↑   149 ÷    150 K    151 ←ШГ
 *  152 С/П  153 В/О  154 БП   155 ШГ→  156 П→x  157 x→П  158 F    159 ПП
 */
#define KEY_MK_0        132
#define KEY_MK_1        134
#define KEY_MK_2        133
#define KEY_MK_3        139
#define KEY_MK_4        137
#define KEY_MK_5        135
#define KEY_MK_6        136
#define KEY_MK_7        131
#define KEY_MK_8        130
#define KEY_MK_9        138
#define KEY_MK_DOT      145 /* .   decimal point */
#define KEY_MK_NEG      146 /* /-/ change sign */
#define KEY_MK_EE       141 /* ВП  enter exponent */
#define KEY_ESC         27  /* ASCII ESC: "back" */
#define KEY_MK_CX       KEY_ESC /* Cx  clear X, sent as ESC (matrix code 140) */
#define KEY_MK_ENTER    148 /* В↑  push X up the stack */
#define KEY_MK_SWAP     147 /* ↔   swap X and Y */
#define KEY_MK_PLUS     142 /* +   */
#define KEY_MK_MINUS    144 /* −   */
#define KEY_MK_MUL      143 /* ×   */
#define KEY_MK_DIV      149 /* ÷   */
#define KEY_MK_F        158 /* F   prefix: upper function of the next key */
#define KEY_MK_K        150 /* K   prefix: extended commands */
#define KEY_MK_X_TO_P   157 /* x→П store X to register */
#define KEY_MK_P_TO_X   156 /* П→x recall register to X */
#define KEY_MK_STEP_FWD 155 /* ШГ→ program step forward */
#define KEY_MK_STEP_BCK 151 /* ←ШГ program step back */
#define KEY_MK_BP       154 /* БП  unconditional jump (goto) */
#define KEY_MK_PP       159 /* ПП  subroutine call / single step */
#define KEY_MK_VO       153 /* В/О return / reset program counter */
#define KEY_MK_SP       152 /* С/П run / stop */
#define KEY_MK_UPDOWN   128 /* ↑↓  built-in memory exchange */
#define KEY_MK_A_UP     129 /* А↑  set built-in memory address */

/* Chords: two keys held together. */
#define KEY_MK_F_K      160 /* F and K: system monitor */
#define KEY_MK_F_ESC    161 /* F and Cx: kill the running program */
#define KEY_MK_F_UPDOWN 162 /* F and ↑↓: screenshot */
#define KEY_MK_F_A_UP   163 /* F and А↑: USB keyboard on / off */
#define KEY_MK_F_BP     164 /* F and БП: start Wi-Fi */

/* Chords belong to the system: programs get them too, and skip them. */
#define KBD_IS_CHORD(code) ((code) >= KEY_MK_F_K)

/* Bring up the I2C bus and the MCP23018 expander, register KBD_DEV_PATH and
 * start the matrix scan task. Call once at startup. */
esp_err_t keyboard_init(void);

/* True if the key with this code (KEY_ESC for Cx; not a chord) is
 * currently held down (debounced). Right
 * after keyboard_init() it waits for the first scans to finish, so it can be
 * used at boot to check for a key held since power-up. */
bool keyboard_key_down(uint8_t code);

/* Hand `code` to every reader of KBD_DEV_PATH as if it had been typed:
 * how another keyboard joins the MK-52 one. A USB keyboard (usbdev) sends
 * ASCII 1-127 for characters and MK-52 codes for the keys programs know
 * (В↑, Cx, П→x, x→П). Any task. */
void keyboard_inject(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif
