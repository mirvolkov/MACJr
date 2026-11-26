/*
 * KEYVIEWER.ELF -- shows every key press: where the key sits on the MK-52
 * keyboard, its code on /dev/kbrd, and the keyboard connector pins it
 * joins (and the MCP23018 expander pins those go to).
 *
 * The keyboard is drawn as it looks from the front, the last key lit.
 * A key is a crossing of a row pin (1, 12, 13, 14) and a column pin
 * (2..11); the codes run 128..159 over those crossings, row by row
 * (keyboard.h, components/hal/keyboard/keyboard.c). Chords (F + key)
 * light both keys. Keys from a USB keyboard show as ASCII.
 *
 * Cx shows Cx; Cx twice in a row exits.
 *
 * The labels are CP866 (\x escapes, the text in the comments). The font
 * has no arrows: ↑ ↓ → ← ↔ are drawn as ^ v > < <->, ÷ × as / *.
 */
#include <stdio.h>
#include <string.h>

#include "keyboard.h"
#include "macjr.h"

#define GRID_ROW 3  /* screen row of keyboard row 1 */
#define INFO_ROW 8  /* first line of the key's details */
#define FOOT_ROW 15

#define C_KEY  "\x1b[0;97;44m" /* a key */
#define C_LIT  "\x1b[0;30;103m" /* the key just pressed */
#define C_OFF  "\x1b[0m"
#define C_HEAD "\x1b[0;93m"
#define C_TEXT "\x1b[0;97m"
#define C_DIM  "\x1b[0;90m"

#define RAW_CX 140 /* matrix code of Cx; /dev/kbrd sends it as KEY_ESC */

typedef struct {
  const char *label; /* 4 columns at most */
  unsigned code;     /* matrix code (Cx: RAW_CX) */
} kv_key_t;

/* The keyboard as seen from the front (the table in keyboard.h):
 *   F    ШГ>  П>x  7    8    9    -    /
 *   K    <ШГ  x>П  4    5    6    +    *
 *   ^v   В/О  БП   1    2    3    <->  В^
 *   А^   С/П  ПП   0    .    /-/  ВП   Cx */
static const kv_key_t s_keys[4][8] = {
  {{"F", 158}, {"\x98\x83>", 155}, {"\x8F>x", 156}, {"7", 131},
   {"8", 130}, {"9", 138}, {"-", 144}, {"/", 149}},
  {{"K", 150}, {"<\x98\x83", 151}, {"x>\x8F", 157}, {"4", 137},
   {"5", 135}, {"6", 136}, {"+", 142}, {"*", 143}},
  {{"^v", 128}, {"\x82/\x8E", 153}, {"\x81\x8F", 154}, {"1", 134},
   {"2", 133}, {"3", 139}, {"<->", 147}, {"\x82^", 148}},
  {{"\x80^", 129}, {"\x91/\x8F", 152}, {"\x8F\x8F", 159}, {"0", 132},
   {".", 145}, {"/-/", 146}, {"\x82\x8F", 141}, {"Cx", RAW_CX}},
};

/* What the system does with each chord (KEY_MK_F_K..KEY_MK_F_ENTER). */
static const struct {
  unsigned code, a, b;
  const char *what;
} s_chords[] = {
  {KEY_MK_F_K, KEY_MK_F, KEY_MK_K, "system monitor"},
  {KEY_MK_F_ESC, KEY_MK_F, RAW_CX, "kill the program"},
  {KEY_MK_F_UPDOWN, KEY_MK_F, KEY_MK_UPDOWN, "screenshot"},
  {KEY_MK_F_A_UP, KEY_MK_F, KEY_MK_A_UP, "USB keyboard on/off"},
  {KEY_MK_F_BP, KEY_MK_F, KEY_MK_BP, "start Wi-Fi"},
  {KEY_MK_F_ENTER, KEY_MK_F, KEY_MK_ENTER, "file operations"},
};

/* Connector pin -> expander pin, as wired (s_rows/s_cols in keyboard.c). */
static const char *mcp_pin(int pin) {
  switch (pin) {
  case 1: return "GPA1";
  case 12: return "GPA6";
  case 13: return "GPA0";
  case 14: return "GPA7";
  case 2: return "GPA2";
  case 3: return "GPB0";
  case 4: return "GPB6";
  case 5: return "GPB2";
  case 6: return "GPA3";
  case 7: return "GPB3";
  case 8: return "GPB1";
  case 9: return "GPB4";
  case 10: return "GPB5";
  case 11: return "GPB7";
  }
  return "?";
}

/* The pins of matrix code 128..159: row pin 1 has keys on column pins 5
 * and 6 only (128, 129); rows 12, 13, 14 on all ten columns. */
static void pins_of(unsigned code, int *row, int *col) {
  unsigned i = code - KBD_KEYCODE_BASE;

  if (i < 2) {
    *row = 1, *col = 5 + (int) i;
  } else {
    *row = 12 + (int) ((i - 2) / 10);
    *col = 2 + (int) ((i - 2) % 10);
  }
}

static macjr_tty_t *s_tty;

static void out(const char *s) {
  macjr_tty_write(s_tty, s, strlen(s));
}

static void line(int row, const char *s) {
  char buf[96];

  snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[K%s" C_OFF, row, s);
  out(buf);
}

/* The keyboard, with keys lit_a and lit_b (matrix codes, 0 = none) lit. */
static void draw_keys(unsigned lit_a, unsigned lit_b) {
  char buf[64];

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 8; c++) {
      const kv_key_t *k = &s_keys[r][c];
      int lit = k->code == lit_a || k->code == lit_b;

      snprintf(buf, sizeof(buf), "\x1b[%d;%dH%s%-4s" C_OFF, GRID_ROW + r, 1 + c * 5,
               lit ? C_LIT : C_KEY, k->label);
      out(buf);
    }
  }
}

static const kv_key_t *find_key(unsigned code, int *row, int *col) {
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 8; c++) {
      if (s_keys[r][c].code == code) {
        *row = r + 1, *col = c + 1;
        return &s_keys[r][c];
      }
    }
  }
  return 0;
}

static void clear_info(void) {
  for (int row = INFO_ROW; row < FOOT_ROW; row++) {
    line(row, "");
  }
}

/* One MK-52 key: matrix code `code`, sent as `sent`. */
static void show_key(unsigned code, int sent) {
  char buf[64];
  int r = 0, c = 0, rp, cp;
  const kv_key_t *k = find_key(code, &r, &c);

  draw_keys(code, 0);
  clear_info();
  pins_of(code, &rp, &cp);
  snprintf(buf, sizeof(buf), C_HEAD " Key:   %s", k ? k->label : "?");
  line(INFO_ROW, buf);
  snprintf(buf, sizeof(buf), C_TEXT " Place: row %d, column %d", r, c);
  line(INFO_ROW + 1, buf);
  if (sent != (int) code)
    snprintf(buf, sizeof(buf), C_TEXT " Code:  %d (ESC), matrix %u", sent, code);
  else
    snprintf(buf, sizeof(buf), C_TEXT " Code:  %u (0x%02X)", code, code);
  line(INFO_ROW + 2, buf);
  snprintf(buf, sizeof(buf), C_TEXT " Pins:  row %d x column %d", rp, cp);
  line(INFO_ROW + 3, buf);
  snprintf(buf, sizeof(buf), C_DIM " MCP:   %s x %s", mcp_pin(rp), mcp_pin(cp));
  line(INFO_ROW + 4, buf);
}

static void show_chord(int i, unsigned code) {
  char buf[64];
  int ar, ac, br, bc, ap, aq, bp, bq;
  const kv_key_t *a = find_key(s_chords[i].a, &ar, &ac);
  const kv_key_t *b = find_key(s_chords[i].b, &br, &bc);

  draw_keys(s_chords[i].a, s_chords[i].b);
  clear_info();
  pins_of(s_chords[i].a, &ap, &aq);
  pins_of(s_chords[i].b, &bp, &bq);
  snprintf(buf, sizeof(buf), C_HEAD " Chord: %s + %s", a->label, b->label);
  line(INFO_ROW, buf);
  snprintf(buf, sizeof(buf), C_TEXT " Does:  %s", s_chords[i].what);
  line(INFO_ROW + 1, buf);
  snprintf(buf, sizeof(buf), C_TEXT " Code:  %u (0x%02X)", code, code);
  line(INFO_ROW + 2, buf);
  snprintf(buf, sizeof(buf), C_TEXT " Pins:  %dx%d + %dx%d", ap, aq, bp, bq);
  line(INFO_ROW + 3, buf);
  snprintf(buf, sizeof(buf), C_DIM " MCP:   %s x %s + %s x %s", mcp_pin(ap), mcp_pin(aq),
           mcp_pin(bp), mcp_pin(bq));
  line(INFO_ROW + 4, buf);
}

/* A key from a USB keyboard: ASCII 1..127. */
static void show_ascii(int code) {
  char buf[64];

  draw_keys(0, 0);
  clear_info();
  if (code > ' ' && code < 127)
    snprintf(buf, sizeof(buf), C_HEAD " Key:   '%c'", code);
  else
    snprintf(buf, sizeof(buf), C_HEAD " Key:   %s",
             code == ' ' ? "space" : code == '\r' || code == '\n' ? "Enter"
             : code == '\b' || code == 127 ? "Backspace" : code == '\t' ? "Tab"
             : "control");
  line(INFO_ROW, buf);
  line(INFO_ROW + 1, C_TEXT " From:  USB keyboard");
  snprintf(buf, sizeof(buf), C_TEXT " Code:  %d (0x%02X)", code, code);
  line(INFO_ROW + 2, buf);
}

static void show_other(int code) {
  char buf[64];

  draw_keys(0, 0);
  clear_info();
  snprintf(buf, sizeof(buf), C_HEAD " Code:  %d (0x%02X)", code, code);
  line(INFO_ROW, buf);
  line(INFO_ROW + 1, C_TEXT " not an MK-52 key");
}

int main(int argc, char *argv[], macjr_tty_t *tty) {
  int last = -1;

  s_tty = tty;
  out("\x1b[?25l\x1b[0m\x1b[2J");
  macjr_tty_header(tty, "Key viewer");
  draw_keys(0, 0);
  line(INFO_ROW, C_DIM " Press a key");
  line(FOOT_ROW, C_DIM " Cx twice: exit");
  macjr_display_flush(tty);

  for (;;) {
    int code = sys_getkey(), i;

    if (code == KEY_ESC) {
      if (last == KEY_ESC)
        break;
      show_key(RAW_CX, KEY_ESC);
    } else if (code >= KBD_KEYCODE_BASE && code < KBD_KEYCODE_BASE + KBD_KEY_COUNT) {
      show_key((unsigned) code, code);
    } else if (KBD_IS_CHORD(code)) {
      for (i = 0; i < (int) (sizeof s_chords / sizeof s_chords[0]); i++)
        if (s_chords[i].code == (unsigned) code)
          break;
      if (i < (int) (sizeof s_chords / sizeof s_chords[0]))
        show_chord(i, (unsigned) code);
      else
        show_other(code);
    } else if (code > 0 && code < 128) {
      show_ascii(code);
    } else {
      show_other(code);
    }
    last = code;
    macjr_display_flush(tty);
  }
  out("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
  return 0;
}
