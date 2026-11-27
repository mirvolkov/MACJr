/*
 * Hardware a program takes and must give back (exec_exports.h): ADC units,
 * their calibration, LEDC (PWM) channels. A program gets these calls
 * through wrappers that note what it took; exec_hw_release() frees what is
 * left when the program ends, however it ends (killed too, when its own
 * cleanup never runs). Otherwise an ADC unit stays claimed by a handle in
 * an unloaded program, and the next adc_oneshot_new_unit() fails; a PWM
 * output keeps running.
 *
 * Program task only (one program at a time), and exec_hw_release() after it.
 */
#include "exec_exports.h"

#include <stdint.h>

#include "driver/ledc.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "exec";

#define MAX_ADC_UNITS 2
#define MAX_CALI      8

static adc_oneshot_unit_handle_t s_adc[MAX_ADC_UNITS];
static adc_cali_handle_t s_cali[MAX_CALI];
static uint32_t s_ledc[LEDC_SPEED_MODE_MAX]; /* bit c: channel c configured */

esp_err_t exec_adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *ret) {
  esp_err_t err = adc_oneshot_new_unit(cfg, ret);
  if (err == ESP_OK) {
    for (int i = 0; i < MAX_ADC_UNITS; i++) {
      if (!s_adc[i]) {
        s_adc[i] = *ret;
        break;
      }
    }
  }
  return err;
}

esp_err_t exec_adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle) {
  for (int i = 0; i < MAX_ADC_UNITS; i++) {
    if (s_adc[i] == handle) {
      s_adc[i] = NULL;
    }
  }
  return adc_oneshot_del_unit(handle);
}

esp_err_t exec_adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg,
                                                    adc_cali_handle_t *ret) {
  esp_err_t err = adc_cali_create_scheme_curve_fitting(cfg, ret);
  if (err == ESP_OK) {
    for (int i = 0; i < MAX_CALI; i++) {
      if (!s_cali[i]) {
        s_cali[i] = *ret;
        return err;
      }
    }
    ESP_LOGW(TAG, "more than %d ADC calibrations: one stays after the program", MAX_CALI);
  }
  return err;
}

esp_err_t exec_ledc_channel_config(const ledc_channel_config_t *cfg) {
  esp_err_t err = ledc_channel_config(cfg);
  if (err == ESP_OK && cfg->speed_mode < LEDC_SPEED_MODE_MAX && cfg->channel < 32) {
    s_ledc[cfg->speed_mode] |= 1u << cfg->channel;
  }
  return err;
}

void exec_hw_release(void) {
  for (int mode = 0; mode < LEDC_SPEED_MODE_MAX; mode++) {
    for (int ch = 0; s_ledc[mode] != 0 && ch < 32; ch++) {
      if (s_ledc[mode] & (1u << ch)) {
        ledc_stop(mode, ch, 0); /* output low */
        s_ledc[mode] &= ~(1u << ch);
      }
    }
  }
  for (int i = 0; i < MAX_CALI; i++) {
    if (s_cali[i]) {
      adc_cali_delete_scheme_curve_fitting(s_cali[i]);
      s_cali[i] = NULL;
    }
  }
  for (int i = 0; i < MAX_ADC_UNITS; i++) {
    if (s_adc[i]) {
      adc_oneshot_del_unit(s_adc[i]);
      s_adc[i] = NULL;
    }
  }
}
