#ifndef MACJR_SPEAKER_H
#define MACJR_SPEAKER_H

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Invoked from the speaker task once a buffer handed to speaker_write() has
 * either finished playing or failed -- never from the caller's own thread.
 * `result` is ESP_OK on a clean finish, or the i2s_channel_write() error
 * otherwise. `ctx` is whatever was passed in to speaker_write(). */
typedef void (*speaker_write_cb_t)(void *ctx, esp_err_t result);

/* Bring up the I2S TX channel (16-bit mono, standard mode) and the
 * background task that drains speaker_write() requests. Safe to call once
 * at startup. */
esp_err_t speaker_init(void);

/* Queue `sample_count` 16-bit mono samples for playback. Returns
 * immediately -- the actual i2s_channel_write() happens on the speaker task,
 * which calls on_complete(ctx, result) when that write returns. `samples`
 * is read by the speaker task, not copied: it must stay valid until
 * on_complete fires. Returns ESP_ERR_NO_MEM/ESP_ERR_INVALID_STATE if the
 * pending-request queue is full or speaker_init() hasn't succeeded yet. */
esp_err_t speaker_write(const int16_t *samples, size_t sample_count, speaker_write_cb_t on_complete, void *ctx);

/* Stop the speaker task and tear down the I2S channel. */
void speaker_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
