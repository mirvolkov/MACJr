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

#endif
