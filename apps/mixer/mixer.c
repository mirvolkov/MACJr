/*
 * MIXER.ELF -- speaker volume control in the style of alsamixer. One vertical
 * bar per channel of the speaker HAL: Master, and after it PCM (WAV files
 * and other programs' sound) and Beep (key clicks). Every key press beeps,
 * so Master and Beep can be judged by ear as they change. Each change is
 * heard at once and saved in the db for the next boot. A bar is green up
 * to 70 %, white above that and red above 90 %; grey when muted.
 *
 *   ═════════════════ Mixer ════════════════
 *    Card: MAX98357A          Item: Master
 *        100           80            30
 *       ┌──┐          ┌──┐          ┌──┐
 *       │██│          │  │          │  │
 *       │██│          │▄▄│          │  │
 *       │██│          │██│          │  │
 *       │██│          │██│          │▄▄│
 *       └──┘          └──┘          └──┘
 *        OO            OO            MM
 *      Master          PCM           Beep
 *    <ШГ ШГ> item  П>x x>П vol  /-/ mute
 *
 * Keys: ←ШГ / ШГ→ pick the channel, П→x / x→П change it by 5 %, + / − by
 * 1 %, 1-9 set 10-90 % and 0 sets 100 %, /-/ mutes, Cx leaves.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "keyboard.h"
#include "macjr.h"
#include "speaker.h"

#define KEY_PREV      KEY_MK_STEP_BCK /* ←ШГ */
#define KEY_NEXT      KEY_MK_STEP_FWD /* ШГ→ */
#define KEY_UP        KEY_MK_P_TO_X   /* П→x */
#define KEY_DOWN      KEY_MK_X_TO_P   /* x→П */
#define KEY_FINE_UP   KEY_MK_PLUS
#define KEY_FINE_DOWN KEY_MK_MINUS
#define KEY_MUTE      KEY_MK_NEG      /* /-/ */
#define KEY_BACK      KEY_ESC         /* Cx */

#define STEP      5
#define FINE_STEP 1
#define LOUD      70 /* % above which the bar turns white */
#define HOT       90 /* ... and red */

#define COL_W      13 /* screen columns per channel */
#define BOX_W      4  /* ┌──┐ */
#define ROW_INFO   2
#define ROW_VALUE  3
#define ROW_TOP    4
#define BAR_ROWS   7  /* two steps per row with ▄: 14 levels */
#define ROW_BOTTOM (ROW_TOP + BAR_ROWS + 1)
#define ROW_MUTE   (ROW_BOTTOM + 1)
#define ROW_LABEL  (ROW_MUTE + 1)
#define ROW_HINT   15

/* CP866 */
#define G_TL    "\xDA" /* ┌ */
#define G_TR    "\xBF" /* ┐ */
#define G_BL    "\xC0" /* └ */
#define G_BR    "\xD9" /* ┘ */
#define G_H     "\xC4" /* ─ */
#define G_V     "\xB3" /* │ */
#define G_FULL  "\xDB" /* █ */
#define G_LOWER "\xDC" /* ▄ */

static const char *TAG = "mixer";

static macjr_tty_t *s_tty;
static int s_sel; /* selected channel */

static void out(const char *s) {
  macjr_tty_write(s_tty, s, strlen(s));
}

/* Move to row r, column c (1-based) and print the formatted text. */
static void at(int r, int c, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void at(int r, int c, const char *fmt, ...) {
  char buf[96];
  int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", r, c);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  out(buf);
}

/* Print `text` centered in `width` columns starting at column c. */
static void centered(int r, int c, int width, const char *style, const char *text) {
  int pad = (width - (int)strlen(text)) / 2;
  at(r, c + (pad > 0 ? pad : 0), "%s%s\x1b[0m", style, text);
}

/* Bar and value color: green up to LOUD %, white above it, red above HOT %. */
static const char *level_color(int vol) {
  return vol > HOT ? "\x1b[0;91m" : vol > LOUD ? "\x1b[0;97m" : "\x1b[0;92m";
}

static void draw_channel(int ch) {
  int c0 = 1 + ch * COL_W;
  int box = c0 + (COL_W - BOX_W) / 2;
  int vol = speaker_volume(ch);
  bool muted = speaker_muted(ch);
  const char *frame = ch == s_sel ? "\x1b[0;93m" : "\x1b[0;37m";
  const char *fill = muted ? "\x1b[0;90m" : level_color(vol);

  char num[8];
  snprintf(num, sizeof(num), "%d", vol);
  centered(ROW_VALUE, box, BOX_W, fill, num);

  at(ROW_TOP, box, "%s" G_TL G_H G_H G_TR, frame);
  int halves = (vol * BAR_ROWS * 2 + SPEAKER_VOLUME_MAX / 2) / SPEAKER_VOLUME_MAX;
  for (int k = 0; k < BAR_ROWS; k++) { /* k = 0 is the bottom row */
    const char *cell = halves >= 2 * (k + 1) ? G_FULL G_FULL : halves == 2 * k + 1 ? G_LOWER G_LOWER : "  ";
    at(ROW_BOTTOM - 1 - k, box, "%s" G_V "%s%s%s" G_V, frame, fill, cell, frame);
  }
  at(ROW_BOTTOM, box, "%s" G_BL G_H G_H G_BR, frame);

  at(ROW_MUTE, box + 1, "%s\x1b[0m", muted ? "\x1b[97;41mMM" : "\x1b[30;42mOO");

  char label[16];
  snprintf(label, sizeof(label), " %s ", speaker_channel_name(ch));
  centered(ROW_LABEL, c0, COL_W, ch == s_sel ? "\x1b[0;97;41m" : "\x1b[0;37m", label);
}

static void draw(void) {
  out("\x1b[0m\x1b[2J");
  macjr_tty_header(s_tty, "Mixer");
  at(ROW_INFO, 1, "\x1b[0;37m Card: \x1b[97mMAX98357A");
  at(ROW_INFO, 28, "\x1b[0;37mItem: \x1b[97m%s", speaker_channel_name(s_sel));
  for (int ch = 0; ch < SPEAKER_CH_COUNT; ch++) {
    draw_channel(ch);
  }
  at(ROW_HINT, 1, "\x1b[0;90m <\x98\x83 \x98\x83> item  \x8F>x x>\x8F vol  /-/ mute\x1b[0m");
  macjr_display_flush(s_tty);
}

/* 1-9 -> 10-90 %, 0 -> 100 %; -1 for any other key. */
static int digit_volume(int key) {
  static const int digit_keys[10] = {
      KEY_MK_0, KEY_MK_1, KEY_MK_2, KEY_MK_3, KEY_MK_4,
      KEY_MK_5, KEY_MK_6, KEY_MK_7, KEY_MK_8, KEY_MK_9,
  };
  for (int d = 0; d < 10; d++) {
    if (key == digit_keys[d]) {
      return d == 0 ? SPEAKER_VOLUME_MAX : d * 10;
    }
  }
  return -1;
}

int main(int argc, char *argv[], macjr_tty_t *tty) {
  (void)argc;
  (void)argv;
  s_tty = tty;
  s_sel = SPEAKER_CH_MASTER;

  for (;;) {
    draw();
    int key = sys_getkey();
    if (KBD_IS_CHORD(key)) {
      continue;
    }
    int vol = speaker_volume(s_sel);
    int set = digit_volume(key);
    switch (key) {
      case KEY_BACK:
        out("\x1b[0m\x1b[2J");
        return 0;
      case KEY_PREV:
        s_sel = s_sel > 0 ? s_sel - 1 : 0;
        continue;
      case KEY_NEXT:
        s_sel = s_sel < SPEAKER_CH_COUNT - 1 ? s_sel + 1 : s_sel;
        continue;
      case KEY_UP:        set = vol + STEP; break;
      case KEY_DOWN:      set = vol - STEP; break;
      case KEY_FINE_UP:   set = vol + FINE_STEP; break;
      case KEY_FINE_DOWN: set = vol - FINE_STEP; break;
      case KEY_MUTE:
        speaker_set_mute(s_sel, !speaker_muted(s_sel));
        set = vol;
        break;
      default:
        if (set < 0) {
          continue; /* not a mixer key */
        }
        break;
    }
    speaker_set_volume(s_sel, set); /* clamps to 0..100 */
    esp_err_t ret = speaker_save_volume(s_sel);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "%s not saved: %s", speaker_channel_name(s_sel), esp_err_to_name(ret));
    }
  }
}
