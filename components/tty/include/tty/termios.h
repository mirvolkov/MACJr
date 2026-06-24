/*
 * Minimal termios subset (POSIX-like). Values are internal; not Linux-binary compatible.
 */
#ifndef MACJR_TERMIOS_H
#define MACJR_TERMIOS_H

#include <stdint.h>

typedef uint32_t tcflag_t;
typedef uint8_t cc_t;

#define MACJR_NCCS 16

#define MACJR_VINTR 0
#define MACJR_VQUIT 1
#define MACJR_VERASE 2
#define MACJR_VKILL 3
#define MACJR_VEOF 4
#define MACJR_VTIME 5
#define MACJR_VMIN 6
#define MACJR_VSUSP 7
#define MACJR_VSTART 8
#define MACJR_VSTOP 9

/* c_iflag */
#define MACJR_IGNBRK (1u << 0)
#define MACJR_BRKINT (1u << 1)
#define MACJR_IGNPAR (1u << 2)
#define MACJR_INPCK (1u << 3)
#define MACJR_ISTRIP (1u << 4)
#define MACJR_INLCR (1u << 5)
#define MACJR_IGNCR (1u << 6)
#define MACJR_ICRNL (1u << 7)
#define MACJR_IXON (1u << 8)
#define MACJR_IXOFF (1u << 9)

/* c_oflag */
#define MACJR_OPOST (1u << 0)
#define MACJR_ONLCR (1u << 1)
#define MACJR_OCRNL (1u << 2)

/* c_lflag */
#define MACJR_ISIG (1u << 0)
#define MACJR_ICANON (1u << 1)
#define MACJR_ECHO (1u << 2)
#define MACJR_ECHOE (1u << 3)
#define MACJR_ECHOK (1u << 4)
#define MACJR_ECHOCTL (1u << 5)
#define MACJR_NOFLSH (1u << 6)
#define MACJR_TOSTOP (1u << 7)

struct macjr_termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_lflag;
  cc_t c_cc[MACJR_NCCS];
};

struct macjr_winsize {
  uint16_t ws_row;
  uint16_t ws_col;
  uint16_t ws_xpixel;
  uint16_t ws_ypixel;
};

#endif
