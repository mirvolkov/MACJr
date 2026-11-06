/*
 * Scrollable list window for console apps: a column of items with a
 * DOS-style scroll bar on its right edge, one item selected.
 *
 *   first item    ▲     arrows at both ends, a checkered track and a
 *   second item   ▒     one-cell thumb (■) that follows the selection
 *   selected item ■     through the list; the selected item is drawn
 *   fourth item   ▒     white on blue
 *   fifth item    ▼
 *
 * The app owns the items; the list asks for an item's text by index when
 * it draws, so a list can be longer than the window. Moving the selection
 * past the window's edge scrolls it.
 */
#ifndef MACJR_TTY_LIST_H
#define MACJR_TTY_LIST_H

#include <stdbool.h>
#include <stddef.h>

#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Write the text of item `index` into `text` (`len` bytes, plain CP866,
 * no escapes). The list cuts it to the window and pads it. */
typedef void (*macjr_tty_list_item_t)(int index, char *text, size_t len, void *ctx);

typedef struct {
  int row, col;      /* top-left cell, 1-based */
  int width, height; /* in cells, the scroll bar (last column) included */
  int count;         /* number of items */
  int sel;           /* selected item, -1 when the list is empty */
  int top;           /* first item on screen */
  macjr_tty_list_item_t item;
  void *ctx;
} macjr_tty_list_t;

/* Set up an empty list in the given window. Draws nothing. */
void macjr_tty_list_init(macjr_tty_list_t *list, int row, int col, int width, int height,
                         macjr_tty_list_item_t item, void *ctx);

/* Change the number of items, keeping the selection where it can be:
 * moved to the last item if it is past the end, to the first if the list
 * was empty. Draws nothing. */
void macjr_tty_list_set_count(macjr_tty_list_t *list, int count);

/* Move the selection by `delta` items (negative: up), stopping at the
 * ends, and scroll to keep it in the window. True if it moved. Draws
 * nothing. */
bool macjr_tty_list_move(macjr_tty_list_t *list, int delta);

/* Draw the window: scroll bar and visible items. Leaves the colors reset;
 * doesn't flush. */
void macjr_tty_list_draw(macjr_tty_t *tty, const macjr_tty_list_t *list);

#ifdef __cplusplus
}
#endif

#endif
