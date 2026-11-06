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
#define COLOR_SELECT "\x1b[0;97;44m"

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

void macjr_tty_list_draw(macjr_tty_t *tty, const macjr_tty_list_t *list) {
  int text_w = list->width - 1;
  if (text_w > ITEM_MAX - 1) {
    text_w = ITEM_MAX - 1;
  }
  for (int y = 0; y < list->height; y++) {
    char buf[ITEM_MAX + 48];
    int i = list->top + y;
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH%s", list->row + y, list->col,
                     i == list->sel ? COLOR_SELECT : COLOR_ITEM);
    char text[ITEM_MAX] = "";
    if (i < list->count) {
      list->item(i, text, sizeof(text), list->ctx);
    }
    int len = (int)strnlen(text, text_w);
    memcpy(buf + n, text, len);
    memset(buf + n + len, ' ', text_w - len);
    n += text_w;
    n += bar_cell(list, y, buf + n, sizeof(buf) - n);
    macjr_tty_write(tty, buf, n);
  }
  const char *reset = "\x1b[0m";
  macjr_tty_write(tty, reset, strlen(reset));
}
