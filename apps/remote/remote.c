/*
 * REMOTE.ELF -- remote control: shows the last IR command received and
 * sends it again on А↑.
 *
 *   ════════════ Remote control ════════════
 *
 *   Protocol  NEC
 *   Address   0x00
 *   Command   0x45
 *   Extra     0x00
 *   Edges     68
 *
 *   Sent      NEC 0x00/0x45
 *
 *    А↑ send again   В↑ exit
 *
 * A protocol the decoder doesn't know is replayed from its raw burst.
 * The receiver also hears what the LED sends, so a sent command shows up
 * as received again.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_err.h"
#include "ir_rx.h"
#include "ir_tx.h"
#include "keyboard.h"
#include "macjr.h"
#include "monitor.h"

#define KEY_SEND  KEY_MK_A_UP /* А↑ */
#define POLL_MS   20
#define ROW_FIRST 3
#define ROW_SENT  10
#define ROW_HINT  15
#define RAW_MAX   128

static macjr_tty_t *s_tty;
static volatile bool s_redraw; /* set when the monitor closes */

static ir_event_t s_last;
static bool s_have_last;
static uint32_t s_raw[RAW_MAX]; /* burst of s_last if it is IR_PROTO_UNKNOWN */
static size_t s_raw_len;
static char s_sent[48]; /* what the "Sent" row says */

static void out(const char *s) {
  macjr_tty_write(s_tty, s, strlen(s));
}

/* Move to row r (1-based), print the formatted text, clear the rest. */
static void line(int r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void line(int r, const char *fmt, ...) {
  char buf[96];
  int n = snprintf(buf, sizeof(buf), "\x1b[%d;1H", r);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  out(buf);
  out("\x1b[K");
}

static void draw_last(void) {
  if (!s_have_last) {
    line(ROW_FIRST, "\x1b[90mWaiting for a remote...");
    return;
  }
  const ir_event_t *ev = &s_last;
  line(ROW_FIRST, "\x1b[96mProtocol  \x1b[97m%s%s", ir_proto_name(ev->proto), ev->repeat ? " (held)" : "");
  if (ev->proto == IR_PROTO_UNKNOWN) {
    line(ROW_FIRST + 1, "\x1b[96mAddress   \x1b[90m-");
    line(ROW_FIRST + 2, "\x1b[96mCommand   \x1b[90m-");
    line(ROW_FIRST + 3, "\x1b[96mExtra     \x1b[90m-");
  } else {
    line(ROW_FIRST + 1, "\x1b[96mAddress   \x1b[97m0x%02X", ev->addr);
    line(ROW_FIRST + 2, "\x1b[96mCommand   \x1b[97m0x%02X", ev->cmd);
    line(ROW_FIRST + 3, "\x1b[96m%-10s\x1b[97m0x%02X", ev->proto == IR_PROTO_RC5 ? "Toggle" : "Extra", ev->ext);
  }
  line(ROW_FIRST + 4, "\x1b[96mEdges     \x1b[97m%u", ev->edges);
}

static void draw_sent(void) {
  line(ROW_SENT, "\x1b[96mSent      \x1b[97m%s", s_sent);
}

static void draw_all(void) {
  out("\x1b[?25l\x1b[0m\x1b[2J");
  macjr_tty_header(s_tty, "Remote control");
  draw_last();
  draw_sent();
  line(ROW_HINT, "\x1b[90m \x80^ send again   \x82^ exit"); /* А↑, В↑ in CP866 */
  macjr_display_flush(s_tty);
}

static void received(const ir_event_t *ev) {
  s_last = *ev;
  s_have_last = true;
  if (ev->proto == IR_PROTO_UNKNOWN) {
    s_raw_len = ir_rx_last_burst(s_raw, RAW_MAX);
    if (s_raw_len > RAW_MAX) {
      s_raw_len = RAW_MAX;
    }
  }
}

static void send_last(void) {
  if (!s_have_last) {
    snprintf(s_sent, sizeof(s_sent), "\x1b[91mnothing received yet");
    return;
  }
  ir_event_t ev = s_last;
  ev.repeat = false; /* the whole command, not an NEC repeat code */
  esp_err_t err = ev.proto == IR_PROTO_UNKNOWN ? ir_tx_send_raw(s_raw, s_raw_len, IR_TX_CARRIER_HZ)
                                               : ir_tx_send(&ev);
  if (err != ESP_OK) {
    snprintf(s_sent, sizeof(s_sent), "\x1b[91mfailed: %s", esp_err_to_name(err));
  } else if (ev.proto == IR_PROTO_UNKNOWN) {
    snprintf(s_sent, sizeof(s_sent), "raw, %u durations", (unsigned)s_raw_len);
  } else {
    snprintf(s_sent, sizeof(s_sent), "%s 0x%02X/0x%02X", ir_proto_name(ev.proto), ev.addr, ev.cmd);
  }
}

static void on_monitor_closed(void *ctx) {
  (void)ctx;
  s_redraw = true;
}

int main(int argc, char *argv[], macjr_tty_t *tty) {
  s_tty = tty;
  s_sent[0] = '\0';
  ir_event_t ev;
  while (ir_rx_read(&ev, 0) == ESP_OK) {
    /* drop what came while the menu was up */
  }

  int fd = open(KBD_DEV_PATH, O_RDONLY);
  if (fd >= 0) {
    fcntl(fd, F_SETFL, O_NONBLOCK);
  }
  monitor_set_redraw_cb(on_monitor_closed, NULL);
  draw_all();

  for (;;) {
    bool dirty = false;
    if (ir_rx_read(&ev, POLL_MS) == ESP_OK) { /* the loop's pace */
      received(&ev);
      dirty = true;
    }
    uint8_t key;
    if (fd >= 0 && read(fd, &key, 1) == 1 && !monitor_is_open()) {
      if (key == KEY_MK_ENTER) {
        break;
      }
      if (key == KEY_SEND) {
        send_last();
        dirty = true;
      }
    }
    if (monitor_is_open()) {
      continue; /* the monitor owns the screen */
    }
    if (s_redraw) {
      s_redraw = false;
      draw_all();
    } else if (dirty) {
      draw_last();
      draw_sent();
      macjr_display_flush(s_tty);
    }
  }
  if (fd >= 0) {
    close(fd);
  }
  return 0;
}
