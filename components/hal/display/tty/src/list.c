/*
 * Scrollable list window (tty/list.h).
 */
#include "tty/list.h"

#include <stdio.h>
#include <string.h>

#define BAR_UP    '\x1E' /* ▲ */
#define BAR_DOWN  '\x1F' /* ▼ */
#define BAR_TRACK '\xB1' /* ▒ */
#define BAR_THUMB '\xFE' /* ■ */

#define COLOR_BAR_CELL  "\x1b[0;30;107m" /* arrows and thumb: black on white */
#define COLOR_BAR_TRACK "\x1b[0;97;40m"
#define COLOR_ITEM   "\x1b[0;37m"
#define COLOR_SELECT "\x1b[0;30;46m" /* black on cyan, as in DOS Navigator */

#define ITEM_MAX 128 /* widest item text, in bytes */

void macjr_tty_list_init(macjr_tty_list_t *list, int row, int col, int width, int height,
                         macjr_tty_list_item_t item, void *ctx) {
  *list = (macjr_tty_list_t){
    .row = row,
    .col = col,
    .width = width,
    .height = height,
    .sel = -1,
    .item = item,
    .ctx = ctx,
  };
}

/* Scroll so that the selection is in the window, and no further. */
static void follow(macjr_tty_list_t *list) {
  if (list->sel < list->top) {
    list->top = list->sel;
  } else if (list->sel >= list->top + list->height) {
    list->top = list->sel - list->height + 1;
  }
  int max_top = list->count - list->height;
  if (list->top > max_top) {
    list->top = max_top;
  }
  if (list->top < 0) {
    list->top = 0;
  }
}

void macjr_tty_list_set_count(macjr_tty_list_t *list, int count) {
  list->count = count < 0 ? 0 : count;
  if (list->count == 0) {
    list->sel = -1;
  } else if (list->sel < 0) {
    list->sel = 0;
  } else if (list->sel >= list->count) {
    list->sel = list->count - 1;
  }
  follow(list);
}

bool macjr_tty_list_move(macjr_tty_list_t *list, int delta) {
  if (list->count == 0) {
    return false;
  }
  int sel = list->sel + delta;
  if (sel < 0) {
    sel = 0;
  } else if (sel >= list->count) {
    sel = list->count - 1;
  }
  if (sel == list->sel) {
    return false;
  }
  list->sel = sel;
  follow(list);
  return true;
}

/* Row of the thumb in the window: on the track between the arrows, as far
 * along as the selection is along the list. */
static int thumb_row(const macjr_tty_list_t *list) {
  int track = list->height - 2;
  if (list->sel <= 0 || list->count <= 1 || track <= 1) {
    return 1;
  }
  return 1 + (list->sel * (track - 1) + (list->count - 1) / 2) / (list->count - 1);
}

/* The scroll bar cell of row `y`, with its colors. */
static int bar_cell(const macjr_tty_list_t *list, int y, char *out, size_t len) {
  if (y == 0 || y == list->height - 1) {
    return snprintf(out, len, COLOR_BAR_CELL "%c", y == 0 ? BAR_UP : BAR_DOWN);
  }
  if (y == thumb_row(list)) {
    return snprintf(out, len, COLOR_BAR_CELL "%c", BAR_THUMB);
  }
  return snprintf(out, len, COLOR_BAR_TRACK "%c", BAR_TRACK);
}

/* Copy item text to `out`, cut and padded to `width` cells. SGR escapes
 * (ESC [ ... final byte) take no cell; they are kept only if `colors`.
 * The number of bytes written. */
static int put_text(char *out, const char *text, int width, bool colors) {
  int n = 0, cells = 0;
  for (const char *p = text; *p && cells < width;) {
    if (p[0] == '\x1b' && p[1] == '[') {
      const char *q = p + 2;
      while (*q && !(*q >= 0x40 && *q <= 0x7E)) {
        q++;
      }
      if (*q) {
        q++; /* the final byte */
      }
      if (colors) {
        memcpy(out + n, p, q - p);
        n += q - p;
      }
      p = q;
      continue;
    }
    out[n++] = *p++;
    cells++;
  }
  memset(out + n, ' ', width - cells);
  return n + width - cells;
}

void macjr_tty_list_draw(macjr_tty_t *tty, const macjr_tty_list_t *list) {
  int text_w = list->width - 1;
  if (text_w > ITEM_MAX - 1) {
    text_w = ITEM_MAX - 1;
  }
  for (int y = 0; y < list->height; y++) {
    char buf[2 * ITEM_MAX + 48]; /* escapes, then padding up to text_w */
    int i = list->top + y;
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH%s", list->row + y, list->col,
                     i == list->sel ? COLOR_SELECT : COLOR_ITEM);
    char text[ITEM_MAX] = "";
    if (i < list->count) {
      list->item(i, text, sizeof(text), list->ctx);
    }
    n += put_text(buf + n, text, text_w, i != list->sel);
    n += bar_cell(list, y, buf + n, sizeof(buf) - n);
    macjr_tty_write(tty, buf, n);
  }
  const char *reset = "\x1b[0m";
  macjr_tty_write(tty, reset, strlen(reset));
}
