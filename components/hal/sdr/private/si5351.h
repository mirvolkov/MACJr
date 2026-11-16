#ifndef MACJR_SI5351_H
#define MACJR_SI5351_H

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/* Minimal Si5351A driver (register map per Skyworks/SiLabs AN619): PLLA
 * feeds CLK0 and CLK1 in integer mode with a phase offset between them.
 * CLK2 and PLLB are left powered down. */

#define SI5351_ADDR    0x60
#define SI5351_XTAL_HZ 25000000u /* reference crystal; 27 MHz on some modules */

/* Probe the chip on `bus`, wait for it to finish power-up, disable and
 * power down all outputs. */
esp_err_t si5351_init(i2c_master_bus_handle_t bus);

/* Put `hz` on CLK0 and CLK1 with CLK1 lagging by 90 degrees. Picks an even
 * integer output divider N (6..126) so the PLL runs at N * hz within
 * 600..900 MHz, sets PHOFF(CLK1) = N, then resets PLLA to align phases. */
esp_err_t si5351_set_quadrature(uint32_t hz);

/* Generator mode: PLLA at SI5351_GEN_VCO_HZ (integer, so no PLL jitter
 * from a fraction), each of CLK0..CLK2 from its own fractional multisynth
 * and R divider. Programs the PLL and resets it; outputs stay as they are. */
#define SI5351_GEN_VCO_HZ 800000000ull /* 32 x 25 MHz */
esp_err_t si5351_gen_start(void);

/* Put `hz` (SDR_GEN_MIN_HZ..SDR_GEN_MAX_HZ) on CLK`ch` (0..2), or power the
 * output down if `hz` is 0. Doesn't touch the output enables. */
esp_err_t si5351_gen_set(int ch, uint32_t hz);

/* Enable the outputs in `mask` (bit n = CLKn), disable the others. */
esp_err_t si5351_output_enable(uint8_t mask);

#endif
