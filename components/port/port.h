#ifndef MACJR_PORT_H
#define MACJR_PORT_H

#include "sdcard.h"
#include "tty/hal_display.h"
#include "tty/tty.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The picture shown: the board's port layout. */
#define PORT_IMAGE_PATH SDCARD_MOUNT_POINT "/IMG/layout.png"

/* Draw PORT_IMAGE_PATH on `display`, scaled to fit the screen and centered
 * (aspect ratio kept). If it can't be shown, say why on `tty`. Returns once
 * the picture is on screen; it stays there. */
void port_show(macjr_tty_t *tty, macjr_hal_display_t *display);

/* Draw the picture again with the arguments of the last port_show(); fits
 * monitor_set_redraw_cb(), since the picture is not part of the tty. */
void port_redraw(void *ctx);

#ifdef __cplusplus
}
#endif

#endif
