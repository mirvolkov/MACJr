/*
 * I2S TX HAL: a background task owns the channel and drains a small queue
 * of speaker_write() requests, so callers get a completion callback instead
 * of blocking on i2s_channel_write() themselves.
 */
#include "speaker.h"

#include <math.h>
#include <stdlib.h>

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
#define BEEP_AMPLITUDE 800 /* of 32767: about 2.4 %, a quiet click */

static const char *TAG = "speaker_hal";

typedef struct {
  const int16_t *samples; /* NULL marks the stop sentinel (or a rate change) */
  uint32_t sample_rate;   /* nonzero: switch the clock to this rate, no samples */
  size_t sample_count;
  speaker_write_cb_t cb;
  void *ctx;
} speaker_request_t;

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

    const uint8_t *bytes = (const uint8_t *)req.samples;
    size_t remaining = req.sample_count * sizeof(int16_t);
    esp_err_t ret = ESP_OK;
    while (remaining > 0) {
      size_t written = 0;
      ret = i2s_channel_write(s_tx_handle, bytes, remaining, &written, portMAX_DELAY);
      if (ret != ESP_OK) {
        break;
      }
      bytes += written;
      remaining -= written;
    }

    if (req.cb) {
      req.cb(req.ctx, ret);
    }
  }
  xSemaphoreGive(s_stopped_sem);
  vTaskDelete(NULL);
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
  esp_err_t ret = speaker_write(tone, n, beep_done, tone);
  if (ret != ESP_OK) {
    free(tone);
  }
  return ret;
}
