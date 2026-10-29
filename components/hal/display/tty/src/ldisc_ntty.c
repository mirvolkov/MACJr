/*
 * Line discipline (the "n_tty" of Linux's tty layer): turns raw keyboard
 * bytes into either a buffered, echoed canonical line (ICANON, the default)
 * or a byte-for-byte raw stream gated by VMIN/VTIME, per macjr_termios.
 */
#include "tty_internal.h"

#include <string.h>
#include <sys/types.h>

/* Wake up any task blocked in macjr_ldisc_read() waiting for a canonical line. */
static void post_line(macjr_tty_t *tty) {
  xSemaphoreGive(tty->line_sem);
}

void macjr_ldisc_init(macjr_ldisc_t *l, macjr_tty_t *owner) {
  memset(l, 0, sizeof(*l));
  l->owner = owner;
}

/* Echo raw bytes back to the console if ECHO is enabled. */
static void echo_bytes(macjr_ldisc_t *l, const uint8_t *p, size_t n) {
  macjr_tty_t *tty = l->owner;
  if (tty->term.c_lflag & MACJR_ECHO) {
    macjr_tty_emit(tty, p, n);
  }
}

/* Visually erase the last echoed character after VERASE: with ECHOE, move
 * the cursor back, overwrite with a space, and move back again; without
 * ECHOE, just emit a bare backspace (matches stty echoe vs noechoe). */
static void echo_erase(macjr_ldisc_t *l) {
  macjr_tty_t *tty = l->owner;
  if (!(tty->term.c_lflag & MACJR_ECHO)) {
    return;
  }
  if (tty->term.c_lflag & MACJR_ECHOE) {
    const uint8_t seq[3] = {'\b', ' ', '\b'};
    macjr_tty_emit(tty, seq, sizeof seq);
  } else {
    const uint8_t bs = '\b';
    macjr_tty_emit(tty, &bs, 1);
  }
}

/* Terminate the in-progress canon_line with '\n' and hand it off as the
 * line ready for macjr_ldisc_read(). Caller holds tty->ldisc_lock. */
static void canon_commit_locked(macjr_ldisc_t *l) {
  if (l->canon_len >= MACJR_LINE_CAP) {
    l->canon_len = MACJR_LINE_CAP - 1;
  }
  l->canon_line[l->canon_len++] = '\n';
  l->canon_read_len = l->canon_len;
  l->canon_len = 0;
  l->canon_ready = true;
}

/* Per-byte canonical/raw-mode state machine, called with bytes fresh from
 * the keyboard driver (macjr_kbd_inject). In ICANON mode each byte is
 * checked against the special control characters (VINTR, VERASE, VEOF) and
 * line terminators before being appended to canon_line; otherwise bytes go
 * straight to the raw queue for macjr_ldisc_read() to drain. */
void macjr_ldisc_rx(macjr_ldisc_t *l, const uint8_t *data, size_t len) {
  macjr_tty_t *tty = l->owner;

  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];

    if (tty->term.c_lflag & MACJR_ICANON) {
      xSemaphoreTake(tty->ldisc_lock, portMAX_DELAY);

      /* VINTR (^C): discard the in-progress line; ISIG gates this since on
       * a real tty it would also signal the foreground process, which this
       * driver doesn't model -- only the line-discard + echo happens here. */
      if ((tty->term.c_lflag & MACJR_ISIG) && c == tty->term.c_cc[MACJR_VINTR]) {
        l->canon_len = 0;
        xSemaphoreGive(tty->ldisc_lock);
        if (tty->term.c_lflag & MACJR_ECHO) {
          const uint8_t caret[] = {'^', 'C'};
          macjr_tty_emit(tty, caret, sizeof caret);
        }
        continue;
      }

      /* VERASE (backspace/DEL): drop the last buffered character. */
      if (c == tty->term.c_cc[MACJR_VERASE]) {
        if (l->canon_len > 0) {
          l->canon_len--;
        }
        xSemaphoreGive(tty->ldisc_lock);
        echo_erase(l);
        continue;
      }

      /* VEOF (^D): on an empty line, signals end-of-file -- deliver a
       * zero-length "line" so macjr_ldisc_read() returns 0. On a
       * non-empty line it's swallowed (matches stty icanon ^D behavior). */
      if (c == tty->term.c_cc[MACJR_VEOF]) {
        if (l->canon_len == 0) {
          l->canon_read_len = 0;
          l->canon_ready = true;
          xSemaphoreGive(tty->ldisc_lock);
          post_line(tty);
        } else {
          xSemaphoreGive(tty->ldisc_lock);
        }
        continue;
      }

      /* CR or LF ends the line; ICRNL maps CR to LF first, same as Linux. */
      if (c == '\r' || c == '\n') {
        if ((tty->term.c_iflag & MACJR_ICRNL) && c == '\r') {
          c = '\n';
        }
        canon_commit_locked(l);
        xSemaphoreGive(tty->ldisc_lock);
        echo_bytes(l, (const uint8_t *)"\r\n", 2);
        post_line(tty);
        continue;
      }

      /* Buffer full: silently drop further characters until the line is read. */
      if (l->canon_len + 1 >= MACJR_LINE_CAP) {
        xSemaphoreGive(tty->ldisc_lock);
        continue;
      }

      l->canon_line[l->canon_len++] = c;
      xSemaphoreGive(tty->ldisc_lock);
      echo_bytes(l, &c, 1);
      continue;
    }

    /* raw mode: no buffering/echo logic beyond ECHO, just queue the byte
     * for macjr_ldisc_read()'s VMIN/VTIME-driven consumer. */
    if (xQueueSend(tty->raw_q, &c, 0) != pdTRUE) {
      /* queue full: drop the byte rather than block the input source */
    }
    if (tty->term.c_lflag & MACJR_ECHO) {
      echo_bytes(l, &c, 1);
    }
  }
}

/* Consume one input unit per the current mode: in ICANON mode, block until
 * a full line is posted (post_line()) and copy at most len bytes of it out;
 * in raw mode, block for VMIN bytes then opportunistically drain up to len. */
ssize_t macjr_ldisc_read(macjr_ldisc_t *l, uint8_t *buf, size_t len) {
  macjr_tty_t *tty = l->owner;

  if (len == 0) {
    return 0;
  }

  if (tty->term.c_lflag & MACJR_ICANON) {
    xSemaphoreTake(tty->line_sem, portMAX_DELAY);

    xSemaphoreTake(tty->ldisc_lock, portMAX_DELAY);

    if (!l->canon_ready) {
      xSemaphoreGive(tty->ldisc_lock);
      return 0;
    }

    /* canon_read_len == 0 is the VEOF-on-empty-line case: report EOF (0). */
    if (l->canon_read_len == 0) {
      l->canon_ready = false;
      xSemaphoreGive(tty->ldisc_lock);
      return 0;
    }

    size_t n = l->canon_read_len;
    if (n > len) {
      n = len;
    }
    memcpy(buf, l->canon_line, n);
    l->canon_ready = false;
    xSemaphoreGive(tty->ldisc_lock);
    return (ssize_t)n;
  }

  /* raw mode: VMIN is the minimum byte count to block for (treated as 1 if
   * 0, since VTIME-based pure-timeout reads aren't implemented); once that
   * many bytes are in hand, grab any additional bytes already queued up
   * without blocking further, up to the caller's buffer size. */
  size_t vmin = tty->term.c_cc[MACJR_VMIN];
  if (vmin == 0) {
    vmin = 1;
  }
  if (vmin > len) {
    vmin = len;
  }

  for (size_t i = 0; i < vmin; i++) {
    if (xQueueReceive(tty->raw_q, &buf[i], portMAX_DELAY) != pdTRUE) {
      return (ssize_t)i;
    }
  }

  size_t out = vmin;
  while (out < len) {
    if (xQueueReceive(tty->raw_q, &buf[out], 0) != pdTRUE) {
      break;
    }
    out++;
  }

  return (ssize_t)out;
}
