#ifndef MACJR_I2C_BUS_H
#define MACJR_I2C_BUS_H

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return the board's shared I2C master bus (SDA=8, SCL=9), creating it on
 * first call. Every driver on that line adds its own device to this bus;
 * the IDF I2C master driver serializes transfers between them. */
esp_err_t i2c_bus_get(i2c_master_bus_handle_t *out);

#ifdef __cplusplus
}
#endif

#endif
