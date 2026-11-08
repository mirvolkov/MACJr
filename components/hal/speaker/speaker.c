/*
 * I2S TX HAL: a background task owns the channel and drains a small queue
 * of speaker_write() requests, so callers get a completion callback instead
 * of blocking on i2s_channel_write() themselves. The MAX98357A has no
 * volume control of its own, so the task scales the samples by the mixer
 * gains (Master x the request's channel) as it plays them.
 */
#include "speaker.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#define REQUEST_QUEUE_LEN 4
#define TASK_STACK_SIZE 4096
#define TASK_PRIORITY 5
#define BEEP_AMPLITUDE 8000 /* of 32767: about 2.4 %, a quiet click */
#define GAIN_UNITY 32768   /* Q15 */
#define SCALE_CHUNK 256    /* samples scaled per i2s_channel_write() */

static const char *TAG = "speaker_hal";

typedef struct {
  const int16_t *samples; /* NULL marks the stop sentinel (or a rate change) */
  uint32_t sample_rate;   /* nonzero: switch the clock to this rate, no samples */
  size_t sample_count;
  speaker_channel_t channel;
  speaker_write_cb_t cb;
  void *ctx;
} speaker_request_t;

static const char *const s_ch_names[SPEAKER_CH_COUNT] = {"Master", "PCM", "Beep"};
static const char *const s_ch_keys[SPEAKER_CH_COUNT] = {
    SPEAKER_DB_PREFIX "master", SPEAKER_DB_PREFIX "pcm", SPEAKER_DB_PREFIX "beep"};
static volatile uint8_t s_volume[SPEAKER_CH_COUNT] = {SPEAKER_VOLUME_MAX, SPEAKER_VOLUME_MAX, SPEAKER_VOLUME_MAX};
static volatile bool s_mute[SPEAKER_CH_COUNT];
static int16_t s_scaled[SCALE_CHUNK]; /* speaker task only */

static i2s_chan_handle_t s_tx_handle;
static QueueHandle_t s_request_q;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_stopped_sem;
static SemaphoreHandle_t s_rate_sem; /* speaker_set_sample_rate() waits on it */
static volatile uint32_t s_rate;

/* Called on the speaker task, so no write is in flight. */
static esp_err_t apply_sample_rate(uint32_t hz) {
  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(hz);
  i2s_channel_disable(s_tx_handle);
  esp_err_t ret = i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg);
  if (ret == ESP_OK) {
    s_rate = hz;
  } else {
    ESP_LOGE(TAG, "Failed to set %lu Hz: %s", (unsigned long)hz, esp_err_to_name(ret));
  }
  i2s_channel_enable(s_tx_handle);
  return ret;
}

/* Q15 gain of one channel: the square of its volume, 0 when muted. */
static int32_t channel_gain(speaker_channel_t ch) {
  if (s_mute[ch]) {
    return 0;
  }
  int32_t v = s_volume[ch];
  return v * v * GAIN_UNITY / (SPEAKER_VOLUME_MAX * SPEAKER_VOLUME_MAX);
}

static esp_err_t write_all(const void *data, size_t bytes) {
  const uint8_t *p = data;
  while (bytes > 0) {
    size_t written = 0;
    esp_err_t ret = i2s_channel_write(s_tx_handle, p, bytes, &written, portMAX_DELAY);
    if (ret != ESP_OK) {
      return ret;
    }
    p += written;
    bytes -= written;
  }
  return ESP_OK;
}

/* Play a request in chunks, each scaled by the gains of that moment, so a
 * volume change is heard within a chunk. Muted audio is still written (as
 * silence) to keep the timing callers rely on. */
static esp_err_t play(const speaker_request_t *req) {
  const int16_t *src = req->samples;
  size_t left = req->sample_count;
  while (left > 0) {
    size_t n = left < SCALE_CHUNK ? left : SCALE_CHUNK;
    int32_t gain = (channel_gain(SPEAKER_CH_MASTER) * channel_gain(req->channel)) >> 15;
    const int16_t *out = src;
    if (gain != GAIN_UNITY) {
      for (size_t i = 0; i < n; i++) {
        s_scaled[i] = (int16_t)((src[i] * gain) >> 15);
      }
      out = s_scaled;
    }
    esp_err_t ret = write_all(out, n * sizeof(int16_t));
    if (ret != ESP_OK) {
      return ret;
    }
    src += n;
    left -= n;
  }
  return ESP_OK;
}

static void speaker_task(void *arg) {
  (void)arg;
  for (;;) {
    speaker_request_t req;
    if (xQueueReceive(s_request_q, &req, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (req.sample_rate) {
      esp_err_t ret = apply_sample_rate(req.sample_rate);
      if (req.cb) {
        req.cb(req.ctx, ret);
      }
      continue;
    }
    if (!req.samples) {
      break; /* stop sentinel */
    }

    esp_err_t ret = play(&req);
    if (req.cb) {
      req.cb(req.ctx, ret);
    }
  }
  xSemaphoreGive(s_stopped_sem);
  vTaskDelete(NULL);
}

/* Saved mixer settings from the db; a channel without one keeps 100 %. */
static void load_volumes(void) {
  for (int ch = 0; ch < SPEAKER_CH_COUNT; ch++) {
    char value[16];
    if (db_read(s_ch_keys[ch], value, sizeof(value)) != ESP_OK) {
      continue;
    }
    speaker_set_volume(ch, atoi(value));
    s_mute[ch] = strstr(value, "mute") != NULL;
    ESP_LOGI(TAG, "%s %d%s", s_ch_names[ch], s_volume[ch], s_mute[ch] ? " (muted)" : "");
  }
}

esp_err_t speaker_init(void) {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  /* Send silence once the queued data runs out; otherwise the DMA keeps
   * replaying its last buffer and a short tone never stops. */
  chan_cfg.auto_clear = true;
  esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
    return ret;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_SAMPLE_RATE),
      .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = GPIO_NUM_41,
          .ws = GPIO_NUM_42,
          .dout = GPIO_NUM_40,
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
          },
      },
  };

  ret = i2s_channel_init_std_mode(s_tx_handle, &std_cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize I2S standard mode: %s", esp_err_to_name(ret));
    goto fail_channel;
  }

  ret = i2s_channel_enable(s_tx_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
    goto fail_channel;
  }

  s_request_q = xQueueCreate(REQUEST_QUEUE_LEN, sizeof(speaker_request_t));
  s_stopped_sem = xSemaphoreCreateBinary();
  s_rate_sem = xSemaphoreCreateBinary();
  s_rate = SPEAKER_SAMPLE_RATE;
  if (!s_request_q || !s_stopped_sem || !s_rate_sem) {
    ESP_LOGE(TAG, "Failed to allocate queue/semaphore");
    ret = ESP_ERR_NO_MEM;
    goto fail_queue;
  }

  if (xTaskCreate(speaker_task, "speaker_hal", TASK_STACK_SIZE, NULL, TASK_PRIORITY, &s_task) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create speaker task");
    ret = ESP_ERR_NO_MEM;
    goto fail_queue;
  }

  load_volumes();
  return ESP_OK;

fail_queue:
  if (s_request_q) {
    vQueueDelete(s_request_q);
    s_request_q = NULL;
  }
  if (s_stopped_sem) {
    vSemaphoreDelete(s_stopped_sem);
    s_stopped_sem = NULL;
  }
  if (s_rate_sem) {
    vSemaphoreDelete(s_rate_sem);
    s_rate_sem = NULL;
  }
fail_channel:
  i2s_del_channel(s_tx_handle);
  s_tx_handle = NULL;
  return ret;
}

esp_err_t speaker_write(const int16_t *samples, size_t sample_count, speaker_write_cb_t on_complete, void *ctx) {
  if (!s_request_q) {
    return ESP_ERR_INVALID_STATE;
  }
  speaker_request_t req = {
      .samples = samples,
      .sample_count = sample_count,
      .channel = SPEAKER_CH_PCM,
      .cb = on_complete,
      .ctx = ctx,
  };
  if (xQueueSend(s_request_q, &req, 0) != pdTRUE) {
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

static void rate_done(void *ctx, esp_err_t result) {
  *(esp_err_t *)ctx = result;
  xSemaphoreGive(s_rate_sem);
}

esp_err_t speaker_set_sample_rate(uint32_t hz) {
  if (!s_request_q || hz == 0) {
    return ESP_ERR_INVALID_STATE;
  }
  if (hz == s_rate) {
    return ESP_OK;
  }
  esp_err_t result = ESP_FAIL;
  speaker_request_t req = {
      .sample_rate = hz,
      .cb = rate_done,
      .ctx = &result,
  };
  xQueueSend(s_request_q, &req, portMAX_DELAY);
  xSemaphoreTake(s_rate_sem, portMAX_DELAY);
  return result;
}

uint32_t speaker_sample_rate(void) {
  return s_rate;
}

void speaker_deinit(void) {
  if (!s_request_q) {
    return;
  }
  speaker_request_t stop = {0};
  xQueueSend(s_request_q, &stop, portMAX_DELAY);
  xSemaphoreTake(s_stopped_sem, portMAX_DELAY);

  vQueueDelete(s_request_q);
  s_request_q = NULL;
  vSemaphoreDelete(s_stopped_sem);
  s_stopped_sem = NULL;
  vSemaphoreDelete(s_rate_sem);
  s_rate_sem = NULL;

  i2s_channel_disable(s_tx_handle);
  i2s_del_channel(s_tx_handle);
  s_tx_handle = NULL;
}

static void beep_done(void *ctx, esp_err_t result) {
  (void)result;
  free(ctx);
}

esp_err_t speaker_beep(uint32_t hz, uint32_t ms) {
  uint32_t rate = s_rate;
  if (!s_request_q || hz == 0 || hz >= rate / 2) {
    return ESP_ERR_INVALID_ARG;
  }
  uint32_t periods = hz * ms / 1000;
  if (periods == 0) {
    periods = 1;
  }
  size_t n = (size_t)((uint64_t)periods * rate / hz);
  int16_t *tone = malloc(n * sizeof(int16_t));
  if (!tone) {
    return ESP_ERR_NO_MEM;
  }
  for (size_t i = 0; i < n; i++) {
    tone[i] = (int16_t)(BEEP_AMPLITUDE * sinf(2.0f * (float)M_PI * hz * i / rate));
  }
  speaker_request_t req = {
      .samples = tone,
      .sample_count = n,
      .channel = SPEAKER_CH_BEEP,
      .cb = beep_done,
      .ctx = tone,
  };
  if (xQueueSend(s_request_q, &req, 0) != pdTRUE) {
    free(tone);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

/* ---- mixer ------------------------------------------------------------- */

static bool channel_valid(speaker_channel_t ch) {
  return (unsigned)ch < SPEAKER_CH_COUNT;
}

const char *speaker_channel_name(speaker_channel_t ch) {
  return channel_valid(ch) ? s_ch_names[ch] : NULL;
}

void speaker_set_volume(speaker_channel_t ch, int volume) {
  if (!channel_valid(ch)) {
    return;
  }
  s_volume[ch] = (uint8_t)(volume < 0 ? 0 : volume > SPEAKER_VOLUME_MAX ? SPEAKER_VOLUME_MAX : volume);
}

int speaker_volume(speaker_channel_t ch) {
  return channel_valid(ch) ? s_volume[ch] : 0;
}

void speaker_set_mute(speaker_channel_t ch, bool mute) {
  if (channel_valid(ch)) {
    s_mute[ch] = mute;
  }
}

bool speaker_muted(speaker_channel_t ch) {
  return channel_valid(ch) && s_mute[ch];
}

esp_err_t speaker_save_volume(speaker_channel_t ch) {
  if (!channel_valid(ch)) {
    return ESP_ERR_INVALID_ARG;
  }
  char value[16];
  snprintf(value, sizeof(value), "%d%s", s_volume[ch], s_mute[ch] ? " mute" : "");
  esp_err_t ret = db_update(s_ch_keys[ch], value);
  if (ret == ESP_ERR_NOT_FOUND) {
    ret = db_create(s_ch_keys[ch], value);
  }
  return ret;
}
