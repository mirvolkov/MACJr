#ifndef MACJR_SDR_H
#define MACJR_SDR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default tuning: 7.055 MHz (40 m band). */
#define SDR_DEFAULT_FREQ_HZ 7055000u

/* Tunable range: the 90-degree I/Q phase offset needs an even integer
 * Si5351 output divider of at most 126, with the PLL at 600..900 MHz. */
#define SDR_MIN_FREQ_HZ 4762000u   /* 600 MHz / 126 */
#define SDR_MAX_FREQ_HZ 150000000u /* 900 MHz / 6   */

/* Find the Si5351 on the shared I2C bus and start the Tayloe detector's
 * local oscillator at SDR_DEFAULT_FREQ_HZ: CLK0 = I (0 deg), CLK1 = Q
 * (90 deg). */
esp_err_t sdr_init(void);

/* Retune both LO outputs to `hz`, keeping CLK1 90 degrees behind CLK0.
 * Resets the PLL, so the output glitches briefly. */
esp_err_t sdr_set_freq(uint32_t hz);

/*
 * Generator mode: the Si5351 as a 3-channel clock generator, CLK0..CLK2
 * each at its own frequency (square wave, 8 mA drive). Replaces the I/Q
 * LO set up by sdr_init(); sdr_init() replaces it in turn. The chip keeps
 * running as last set after the program that set it ends.
 */
#define SDR_GEN_CHANNELS 3
#define SDR_GEN_MIN_HZ   8000u      /* 800 MHz / 2048 / R 128 is lower, but below spec */
#define SDR_GEN_MAX_HZ   100000000u /* 800 MHz / 8, the smallest fractional divider */

/* Find the Si5351 on the shared I2C bus and start it as a generator: all
 * channels off (frequency 0), outputs disabled. */
esp_err_t sdr_gen_init(void);

/* Set channel `ch` (0..2) to `hz`, SDR_GEN_MIN_HZ..SDR_GEN_MAX_HZ, or 0 to
 * turn the channel off. A channel gives a signal while it has a frequency
 * and the outputs are enabled. ESP_ERR_INVALID_ARG out of range. */
esp_err_t sdr_gen_set_freq(int ch, uint32_t hz);

/* Enable (`on`) or disable all outputs of the chip at once; channels keep
 * their frequencies. */
esp_err_t sdr_gen_enable(bool on);

#ifdef __cplusplus
}
#endif

#endif
