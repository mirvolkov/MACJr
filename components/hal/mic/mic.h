#ifndef MACJR_MIC_H
#define MACJR_MIC_H

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Left channel is ADC1_CH3 (GPIO4), right channel is ADC1_CH4 (GPIO5). */
#define MIC_CHANNELS 2
#define MIC_SAMPLE_RATE 16000 /* Hz, per channel */

/* Start continuous (DMA) ADC conversion on both mic inputs, alternating
 * L/R at MIC_SAMPLE_RATE per channel. Safe to call once at startup. */
esp_err_t mic_init(void);

/* Fill `frames` with up to `frame_count` interleaved stereo frames
 * (L, R, L, R, ... -- 16-bit signed, DC removed). Blocks until all frames
 * are collected or no ADC data arrived for `timeout_ms`. `*frames_read`
 * receives the number of frames actually written. Not thread-safe: only one
 * reader at a time. */
esp_err_t mic_read(int16_t *frames, size_t frame_count, uint32_t timeout_ms, size_t *frames_read);

/* Stop conversion and release the ADC. */
void mic_deinit(void);

/* Bring up USB OTG as a UAC 2.0 device exposing a 2-channel microphone and
 * stream mic_read() data to the host. Call after mic_init(). Takes over the
 * USB PHY from USB-Serial-JTAG, so the JTAG console/flashing port on the
 * native USB connector disappears once this runs. */
esp_err_t mic_uac_start(void);

#ifdef __cplusplus
}
#endif

#endif
