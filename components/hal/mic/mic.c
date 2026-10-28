/*
 * Two-channel mic HAL: ADC1 runs in continuous (DMA) mode alternating
 * between the left and right inputs. mic_read() pairs the results into
 * interleaved stereo frames, scales 12-bit raw values to 16-bit and strips
 * the analog bias with a one-pole DC-blocking filter per channel.
 */
#include "mic.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_adc/adc_continuous.h"

#define MIC_ADC_UNIT ADC_UNIT_1
#define MIC_CH_LEFT ADC_CHANNEL_2  /* GPIO3 */
#define MIC_CH_RIGHT ADC_CHANNEL_3 /* GPIO4 */

#define RESULT_BYTES SOC_ADC_DIGI_RESULT_BYTES
#define CONV_FRAME_RESULTS 32 /* DMA frame: 16 stereo frames = 1 ms */
#define CONV_FRAME_BYTES (CONV_FRAME_RESULTS * RESULT_BYTES)
#define STORE_BUF_BYTES (CONV_FRAME_BYTES * 8)

#define RAW_MID 2048 /* 12-bit midscale */
#define DC_POLE 32604 /* 0.995 in Q15: ~13 Hz high-pass corner at 16 kHz */

static const char *TAG = "mic_hal";

typedef struct {
  int32_t x_prev;
  int32_t y_prev;
} dc_block_t;

static adc_continuous_handle_t s_adc;
static uint8_t s_raw[CONV_FRAME_BYTES];
static adc_continuous_data_t s_parsed[CONV_FRAME_RESULTS];
static dc_block_t s_dc[MIC_CHANNELS];
static int16_t s_pending_left;
static bool s_have_left;

static int16_t condition_sample(dc_block_t *dc, uint32_t raw) {
  int32_t x = ((int32_t)raw - RAW_MID) << 4; /* 12 -> 16 bit */
  int32_t y = x - dc->x_prev + ((dc->y_prev * DC_POLE) >> 15);
  dc->x_prev = x;
  dc->y_prev = y;
  if (y > INT16_MAX) {
    return INT16_MAX;
  }
  if (y < INT16_MIN) {
    return INT16_MIN;
  }
  return (int16_t)y;
}

esp_err_t mic_init(void) {
  adc_continuous_handle_cfg_t handle_cfg = {
      .max_store_buf_size = STORE_BUF_BYTES,
      .conv_frame_size = CONV_FRAME_BYTES,
      .flags.flush_pool = 1, /* drop stale audio while nobody is reading */
  };
  esp_err_t ret = adc_continuous_new_handle(&handle_cfg, &s_adc);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "adc_continuous_new_handle failed: %s", esp_err_to_name(ret));
    return ret;
  }

  adc_digi_pattern_config_t pattern[MIC_CHANNELS] = {
      {.atten = ADC_ATTEN_DB_12, .channel = MIC_CH_LEFT, .unit = MIC_ADC_UNIT, .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH},
      {.atten = ADC_ATTEN_DB_12, .channel = MIC_CH_RIGHT, .unit = MIC_ADC_UNIT, .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH},
  };
  adc_continuous_config_t adc_cfg = {
      .pattern_num = MIC_CHANNELS,
      .adc_pattern = pattern,
      .sample_freq_hz = MIC_SAMPLE_RATE * MIC_CHANNELS, /* total conversions/s */
      .conv_mode = ADC_CONV_SINGLE_UNIT_1,
  };
  ret = adc_continuous_config(s_adc, &adc_cfg);
  if (ret == ESP_OK) {
    ret = adc_continuous_start(s_adc);
  }
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "ADC start failed: %s", esp_err_to_name(ret));
    adc_continuous_deinit(s_adc);
    s_adc = NULL;
    return ret;
  }

  ESP_LOGI(TAG, "Mic ready: %d Hz x %d ch (L=GPIO3, R=GPIO4)", MIC_SAMPLE_RATE, MIC_CHANNELS);
  return ESP_OK;
}

esp_err_t mic_read(int16_t *frames, size_t frame_count, uint32_t timeout_ms, size_t *frames_read) {
  if (!s_adc) {
    return ESP_ERR_INVALID_STATE;
  }

  size_t got = 0;
  esp_err_t ret = ESP_OK;
  while (got < frame_count) {
    size_t want = (frame_count - got) * MIC_CHANNELS;
    if (want > CONV_FRAME_RESULTS) {
      want = CONV_FRAME_RESULTS;
    }
    uint32_t raw_len = 0;
    ret = adc_continuous_read(s_adc, s_raw, want * RESULT_BYTES, &raw_len, timeout_ms);
    if (ret != ESP_OK) {
      break; /* ESP_ERR_TIMEOUT: hand back what we have */
    }

    uint32_t parsed = 0;
    ret = adc_continuous_parse_data(s_adc, s_raw, raw_len, s_parsed, &parsed);
    if (ret != ESP_OK) {
      break;
    }
    for (uint32_t i = 0; i < parsed && got < frame_count; i++) {
      const adc_continuous_data_t *d = &s_parsed[i];
      if (!d->valid) {
        continue;
      }
      if (d->channel == MIC_CH_LEFT) {
        s_pending_left = condition_sample(&s_dc[0], d->raw_data);
        s_have_left = true;
      } else if (d->channel == MIC_CH_RIGHT && s_have_left) {
        frames[got * 2] = s_pending_left;
        frames[got * 2 + 1] = condition_sample(&s_dc[1], d->raw_data);
        s_have_left = false;
        got++;
      }
    }
  }

  if (frames_read) {
    *frames_read = got;
  }
  return (got > 0 && ret == ESP_ERR_TIMEOUT) ? ESP_OK : ret;
}

void mic_deinit(void) {
  if (!s_adc) {
    return;
  }
  adc_continuous_stop(s_adc);
  adc_continuous_deinit(s_adc);
  s_adc = NULL;
}
