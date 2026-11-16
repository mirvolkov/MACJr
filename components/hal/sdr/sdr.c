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

static bool s_ready;     /* I/Q LO mode */
static bool s_gen;       /* generator mode */
static bool s_gen_on;    /* outputs enabled */
static uint32_t s_gen_hz[SDR_GEN_CHANNELS];

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
    s_gen = false;
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

/* ---- generator mode ---- */

/* Outputs that give a signal: enabled, with a frequency. */
static esp_err_t gen_apply_enable(void)
{
    uint8_t mask = 0;
    for (int ch = 0; ch < SDR_GEN_CHANNELS; ch++) {
        if (s_gen_on && s_gen_hz[ch]) {
            mask |= 1u << ch;
        }
    }
    return si5351_output_enable(mask);
}

esp_err_t sdr_gen_init(void)
{
    i2c_master_bus_handle_t bus;
    esp_err_t ret = i2c_bus_get(&bus);
    if (ret != ESP_OK) {
        return ret;
    }
    s_ready = s_gen = false;
    if ((ret = si5351_init(bus)) != ESP_OK) { /* all outputs off */
        return ret;
    }
    if ((ret = si5351_gen_start()) != ESP_OK) {
        return ret;
    }
    s_gen = true;
    s_gen_on = false;
    for (int ch = 0; ch < SDR_GEN_CHANNELS; ch++) {
        s_gen_hz[ch] = 0;
    }
    ESP_LOGI(TAG, "Si5351 generator ready");
    return ESP_OK;
}

esp_err_t sdr_gen_set_freq(int ch, uint32_t hz)
{
    if (!s_gen) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ch < 0 || ch >= SDR_GEN_CHANNELS || (hz && (hz < SDR_GEN_MIN_HZ || hz > SDR_GEN_MAX_HZ))) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = si5351_gen_set(ch, hz);
    if (ret != ESP_OK) {
        return ret;
    }
    s_gen_hz[ch] = hz;
    return gen_apply_enable();
}

esp_err_t sdr_gen_enable(bool on)
{
    if (!s_gen) {
        return ESP_ERR_INVALID_STATE;
    }
    s_gen_on = on;
    return gen_apply_enable();
}
