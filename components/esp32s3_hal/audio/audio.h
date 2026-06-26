#ifndef MACJR_AUDIO_H
#define MACJR_AUDIO_H

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Invoked from the audio task once a buffer handed to audio_write() has
 * either finished playing or failed -- never from the caller's own thread.
 * `result` is ESP_OK on a clean finish, or the i2s_channel_write() error
 * otherwise. `ctx` is whatever was passed in to audio_write(). */
typedef void (*audio_write_cb_t)(void *ctx, esp_err_t result);

/* Bring up the I2S TX channel (16-bit mono, standard mode) and the
 * background task that drains audio_write() requests. Safe to call once
 * at startup. */
esp_err_t audio_init(void);

/* Queue `sample_count` 16-bit mono samples for playback. Returns
 * immediately -- the actual i2s_channel_write() happens on the audio task,
 * which calls on_complete(ctx, result) when that write returns. `samples`
 * is read by the audio task, not copied: it must stay valid until
 * on_complete fires. Returns ESP_ERR_NO_MEM/ESP_ERR_INVALID_STATE if the
 * pending-request queue is full or audio_init() hasn't succeeded yet. */
esp_err_t audio_write(const int16_t *samples, size_t sample_count, audio_write_cb_t on_complete, void *ctx);

/* Stop the audio task and tear down the I2S channel. */
void audio_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
