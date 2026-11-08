#ifndef MACJR_SPEAKER_H
#define MACJR_SPEAKER_H

#include "esp_err.h"

#include <stdbool.h>
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

/* Rate the speaker starts at, and goes back to after wav_play(). */
#define SPEAKER_SAMPLE_RATE 8000

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

/* Switch the I2S clock to `hz`. The change is queued after the samples
 * already handed to speaker_write(), and the call blocks until those have
 * played and the new clock runs. One caller at a time. */
esp_err_t speaker_set_sample_rate(uint32_t hz);

/* The sample rate the speaker is playing at right now. */
uint32_t speaker_sample_rate(void);

/* Stop the speaker task and tear down the I2S channel. */
void speaker_deinit(void);

/* Queue a sine beep of `hz` for about `ms` (whole periods, so it starts
 * and ends at zero: no click), at a quiet fixed level. Returns at once;
 * the tone is generated at the current sample rate into its own buffer,
 * freed once played. Plays on SPEAKER_CH_BEEP. */
esp_err_t speaker_beep(uint32_t hz, uint32_t ms);

/*
 * Mixer. Every sound plays on one channel, PCM (speaker_write(), WAV
 * files) or Beep (speaker_beep(): key clicks), and goes through Master
 * after it. Each channel has a volume of 0..100 % and a mute switch,
 * applied by the speaker task as it plays, so a change is heard at once.
 * The gain follows the square of the volume, so 50 % is about -12 dB and
 * the steps sound roughly even. speaker_init() loads the saved settings
 * from the db (SPEAKER_DB_PREFIX + lower-case channel name, "80" or
 * "80 mute"); everything starts at 100 % without them.
 */
typedef enum {
  SPEAKER_CH_MASTER,
  SPEAKER_CH_PCM,
  SPEAKER_CH_BEEP,
  SPEAKER_CH_COUNT,
} speaker_channel_t;

#define SPEAKER_VOLUME_MAX 100
#define SPEAKER_DB_PREFIX "speaker."

/* "Master", "PCM", "Beep"; NULL for a channel that doesn't exist. */
const char *speaker_channel_name(speaker_channel_t ch);

/* Set the volume of `ch`, clamped to 0..SPEAKER_VOLUME_MAX. */
void speaker_set_volume(speaker_channel_t ch, int volume);
int speaker_volume(speaker_channel_t ch);

void speaker_set_mute(speaker_channel_t ch, bool mute);
bool speaker_muted(speaker_channel_t ch);

/* Store the volume and mute of `ch` in the db, to be loaded at the next
 * boot. ESP_ERR_INVALID_STATE without the db (no SD card). */
esp_err_t speaker_save_volume(speaker_channel_t ch);

#ifdef __cplusplus
}
#endif

#endif
