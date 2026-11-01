#ifndef MACJR_IR_RX_H
#define MACJR_IR_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Remote control protocols ir_rx recognizes. */
typedef enum {
  IR_PROTO_NEC,     /* 9 ms leader, 32 bits, repeat codes while held */
  IR_PROTO_SAMSUNG, /* NEC timing with a 4.5 ms leader, frame resent while held */
  IR_PROTO_SONY,    /* SIRC 12, 15 or 20 bits */
  IR_PROTO_RC5,     /* Philips RC-5 */
  IR_PROTO_UNKNOWN, /* a burst none of the above decoded; see ir_rx_last_burst() */
} ir_proto_t;

/* One button press as received. */
typedef struct {
  ir_proto_t proto;
  bool repeat;    /* NEC: button held (repeat code); addr/cmd are the last frame's */
  uint16_t addr;  /* NEC/Samsung: 8 bit, or 16 bit if the two address bytes don't
                   * check; Sony: 5 bit (12/20 bit frames) or 8 bit (15 bit); RC-5: 5 bit */
  uint8_t cmd;    /* NEC/Samsung: 8 bit; Sony: 7 bit; RC-5: 7 bit (field bit as bit 6) */
  uint8_t ext;    /* Sony 20 bit: 8 bit extension; RC-5: toggle bit; else 0 */
  uint16_t edges; /* edges in the burst */
} ir_event_t;

#define IR_WAIT_FOREVER UINT32_MAX

/* Start receiving on the IR demodulator output (GPIO5): an edge interrupt
 * and a decoder task. Events queue up until read; when the queue is full
 * new ones are dropped. Call once. */
esp_err_t ir_rx_init(void);

/* Take the next event, waiting up to `timeout_ms` (IR_WAIT_FOREVER: no
 * limit). ESP_ERR_TIMEOUT if none came, ESP_ERR_INVALID_STATE before
 * ir_rx_init(). One reader at a time. */
esp_err_t ir_rx_read(ir_event_t *ev, uint32_t timeout_ms);

/* Copy the durations (us, mark and space alternating, starting with the
 * first mark) of the last burst received, up to the first gap over 10 ms,
 * into `us` (at most `max`). Returns how many there were (may exceed
 * `max`). For learning remotes the decoder doesn't know. */
size_t ir_rx_last_burst(uint32_t *us, size_t max);

/* "NEC", "Samsung", "Sony", "RC-5" or "unknown". */
const char *ir_proto_name(ir_proto_t proto);

#ifdef __cplusplus
}
#endif

#endif
