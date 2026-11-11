/*
 * ESP32-S3 Wi-Fi-band receiver as an SDR (2.4 GHz). The PHY's factory
 * ADC-dump debug engine copies raw complex IQ into a reserved 64 KB SRAM
 * aperture, which the CPU then reads. Mechanism documented by the ESPARGOS
 * esp-sdr project (docs/how-it-works.md); this is a minimal snapshot-only
 * backend for the SDR spectrogram app (apps/sdr). Snapshot, not continuous.
 */
#ifndef MACJR_ESPSDR_H
#define MACJR_ESPSDR_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPSDR_MAX_WORDS 16380u /* IQ words per snapshot (64 KB / 4) */

typedef enum { ESPSDR_RATE_80M = 0, ESPSDR_RATE_40M = 1, ESPSDR_RATE_16M = 6 } espsdr_rate_t;

/* Take the radio for capture. Wi-Fi must already be started. Puts the PHY in
 * promiscuous RX with forced gain; normal Wi-Fi networking is suspended until
 * espsdr_stop(). */
esp_err_t espsdr_start(void);

/* Tune to `mhz` (2.4 GHz channel centers work best: 2412..2472, 2484). */
void espsdr_tune(unsigned mhz);

/* One snapshot of `n` IQ words (<= ESPSDR_MAX_WORDS) at `rate` into `out`
 * (each word: bits 0-9 I, 10-19 Q, 10-bit signed; 20-27 gain). Returns the
 * count, or <0 on timeout/incomplete capture. About 0.2 ms. */
int espsdr_snapshot(uint32_t *out, unsigned n, espsdr_rate_t rate);

/* Release the radio back to normal Wi-Fi. */
void espsdr_stop(void);

#ifdef __cplusplus
}
#endif

#endif
