/*
 * I2S TX HAL: a background task owns the channel and drains a small queue
 * of speaker_write() requests, so callers get a completion callback instead
 * of blocking on i2s_channel_write() themselves.
 */
#include "speaker.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#define SAMPLE_RATE 8000 /* Hz */
#define REQUEST_QUEUE_LEN 4
#define TASK_STACK_SIZE 4096
#define TASK_PRIORITY 5

static const char *TAG = "speaker_hal";

typedef struct {
  const int16_t *samples; /* NULL marks the stop sentinel */
  size_t sample_count;
  speaker_write_cb_t cb;
  void *ctx;
} speaker_request_t;

static i2s_chan_handle_t s_tx_handle;
static QueueHandle_t s_request_q;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_stopped_sem;

static void speaker_task(void *arg) {
  (void)arg;
  for (;;) {
    speaker_request_t req;
    if (xQueueReceive(s_request_q, &req, portMAX_DELAY) != pdTRUE) {
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
  esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
    return ret;
  }

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
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
  if (!s_request_q || !s_stopped_sem) {
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

  i2s_channel_disable(s_tx_handle);
  i2s_del_channel(s_tx_handle);
  s_tx_handle = NULL;
}
