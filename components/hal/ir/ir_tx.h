#ifndef MACJR_IR_TX_H
#define MACJR_IR_TX_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ir_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Carrier for raw bursts of unknown protocols. */
#define IR_TX_CARRIER_HZ 38000

/* Bring up the IR LED output (GPIO4, active high) on an RMT channel that
 * modulates the carrier in hardware. Call once. */
esp_err_t ir_tx_init(void);

/* Send `ev` as received by ir_rx_read(): NEC and Samsung once (NEC
 * repeat code if ev->repeat), Sony three times 45 ms apart as remotes do,
 * RC-5 once. Blocks until sent. ESP_ERR_NOT_SUPPORTED for
 * IR_PROTO_UNKNOWN (send its burst with ir_tx_send_raw()),
 * ESP_ERR_INVALID_STATE before ir_tx_init(). Thread-safe. */
esp_err_t ir_tx_send(const ir_event_t *ev);

/* Send `n` durations (us, mark and space alternating, mark first, as
 * ir_rx_last_burst() gives them) on a `carrier_hz` carrier. At most 128
 * durations, each under 32.768 ms. Blocks until sent. */
esp_err_t ir_tx_send_raw(const uint32_t *us, size_t n, uint32_t carrier_hz);

#ifdef __cplusplus
}
#endif

#endif
