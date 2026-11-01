#ifndef MACJR_REMOTECONTROL_H
#define MACJR_REMOTECONTROL_H

#include "macjr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Program entry (macjr_main_t), run from the boot menu as a built-in. */
int remotecontrol_main(int argc, char *argv[], macjr_tty_t *tty);

#ifdef __cplusplus
}
#endif

#endif
