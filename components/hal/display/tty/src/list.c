/*
 * Scrollable list window (tty/list.h).
 */
#include "tty/list.h"

#include <stdio.h>
#include <string.h>

#define BAR_TRACK '\xB0' /* ░ */
#define BAR_THUMB '\xDB' /* █ */

#define COLOR_BAR    "\x1b[0;36m"
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

/* Rows of the scroll bar taken by the thumb: its first row and count. */
static void thumb(const macjr_tty_list_t *list, int *first, int *size) {
  int h = list->height;
  if (list->count <= h) {
    *first = 0;
    *size = h; /* everything fits */
    return;
  }
  *size = h * h / list->count;
  if (*size < 1) {
    *size = 1;
  }
  int max_top = list->count - h;
  *first = (list->top * (h - *size) + max_top / 2) / max_top;
}

void macjr_tty_list_draw(macjr_tty_t *tty, const macjr_tty_list_t *list) {
  int text_w = list->width - 1;
  if (text_w > ITEM_MAX - 1) {
    text_w = ITEM_MAX - 1;
  }
  int thumb_first, thumb_size;
  thumb(list, &thumb_first, &thumb_size);

  for (int y = 0; y < list->height; y++) {
    char buf[ITEM_MAX + 48];
    int i = list->top + y;
    bool on_thumb = y >= thumb_first && y < thumb_first + thumb_size;
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
    n += snprintf(buf + n, sizeof(buf) - n, COLOR_BAR "%c", on_thumb ? BAR_THUMB : BAR_TRACK);
    macjr_tty_write(tty, buf, n);
  }
  const char *reset = "\x1b[0m";
  macjr_tty_write(tty, reset, strlen(reset));
}
