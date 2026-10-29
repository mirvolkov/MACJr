#ifndef MACJR_WAV_H
#define MACJR_WAV_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Play a WAV file on the speaker; blocks until it has played to the end.
 * Integer PCM, 8/16/24/32-bit, any number of channels (mixed down to
 * mono), at the file's own sample rate; the speaker goes back to
 * SPEAKER_SAMPLE_RATE afterwards. `volume` is 0..100 percent of the
 * file's level. Needs speaker_init() first.
 * ESP_ERR_NOT_FOUND: no such file, ESP_ERR_NOT_SUPPORTED: not a WAV file
 * or a format above (float, compressed). */
esp_err_t wav_play(const char *path, int volume);

#ifdef __cplusplus
}
#endif

#endif
