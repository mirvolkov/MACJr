#ifndef MACJR_SDR_H
#define MACJR_SDR_H

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

#ifdef __cplusplus
}
#endif

#endif
