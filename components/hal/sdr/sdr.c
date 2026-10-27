/*
 * SDR HAL: a Tayloe (quadrature sampling) detector clocked by a Si5351.
 * CLK0 drives the I phase, CLK1 the Q phase, both at the receive frequency
 * with CLK1 90 degrees behind CLK0. The Si5351 shares the I2C bus with the
 * keyboard expander.
 */
#include "sdr.h"

#include <stdbool.h>

#include "esp_log.h"
#include "i2c_bus.h"
#include "si5351.h"

static const char *TAG = "sdr";

static bool s_ready;

esp_err_t sdr_init(void)
{
    i2c_master_bus_handle_t bus;
    esp_err_t ret = i2c_bus_get(&bus);
    if (ret != ESP_OK) {
        return ret;
    }
    if ((ret = si5351_init(bus)) != ESP_OK) {
        return ret;
    }
    s_ready = true;
    if ((ret = sdr_set_freq(SDR_DEFAULT_FREQ_HZ)) != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "SDR LO running at %lu Hz", (unsigned long)SDR_DEFAULT_FREQ_HZ);
    return ESP_OK;
}

esp_err_t sdr_set_freq(uint32_t hz)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (hz < SDR_MIN_FREQ_HZ || hz > SDR_MAX_FREQ_HZ) {
        return ESP_ERR_INVALID_ARG;
    }
    return si5351_set_quadrature(hz);
}
