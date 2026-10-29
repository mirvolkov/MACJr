/*
 * WAV player: parses the RIFF header, then streams the "data" chunk through
 * a few buffers that take turns on the speaker queue. A semaphore counts
 * the free buffers: speaker_write()'s callback gives one back once played,
 * the reader takes one before filling it, so reading the file runs while
 * the previous buffers play.
 */
#include "wav.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "speaker.h"

#define WAV_BUFS 3          /* in flight; the speaker queue holds 4 */
#define WAV_BUF_FRAMES 1024 /* per buffer: 23 ms at 44.1 kHz */

#define WAV_FORMAT_PCM 1
#define WAV_FORMAT_EXTENSIBLE 0xFFFE

static const char *TAG = "wav";

typedef struct {
  uint16_t format;
  uint16_t channels;
  uint32_t rate;
  uint16_t block_align; /* bytes per frame, all channels */
  uint16_t bits;
} wav_fmt_t;

typedef struct {
  SemaphoreHandle_t free_bufs;
  volatile esp_err_t error; /* first failed write */
} wav_play_t;

static uint16_t le16(const uint8_t *p) {
  return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Read the RIFF header and chunks up to "data"; leave `f` at the first
 * sample and return the data size (0xFFFFFFFF if unknown: play to EOF). */
static esp_err_t parse_header(FILE *f, wav_fmt_t *fmt, uint32_t *data_size) {
  uint8_t buf[40];
  if (fread(buf, 1, 12, f) != 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
    return ESP_ERR_NOT_SUPPORTED;
  }
  bool have_fmt = false;
  for (;;) {
    if (fread(buf, 1, 8, f) != 8) {
      return ESP_ERR_NOT_SUPPORTED; /* no "data" chunk */
    }
    uint32_t len = le32(buf + 4);
    if (memcmp(buf, "fmt ", 4) == 0) {
      if (len < 16) {
        return ESP_ERR_NOT_SUPPORTED;
      }
      size_t n = len < sizeof(buf) ? len : sizeof(buf);
      if (fread(buf, 1, n, f) != n) {
        return ESP_ERR_NOT_SUPPORTED;
      }
      fmt->format = le16(buf);
      fmt->channels = le16(buf + 2);
      fmt->rate = le32(buf + 4);
      fmt->block_align = le16(buf + 12);
      fmt->bits = le16(buf + 14);
      /* WAVE_FORMAT_EXTENSIBLE: the real format is the first two bytes of
       * the sub-format GUID at offset 24 */
      if (fmt->format == WAV_FORMAT_EXTENSIBLE && n >= 26) {
        fmt->format = le16(buf + 24);
      }
      len -= n;
      have_fmt = true;
    } else if (memcmp(buf, "data", 4) == 0) {
      if (!have_fmt) {
        return ESP_ERR_NOT_SUPPORTED;
      }
      *data_size = len;
      return ESP_OK;
    }
    /* skip the rest of this chunk; chunks are padded to an even size */
    if (fseek(f, (long)len + (len & 1), SEEK_CUR) != 0) {
      return ESP_ERR_NOT_SUPPORTED;
    }
  }
}

/* One frame of any integer PCM to a mono 16-bit sample at `volume` %. */
static int16_t frame_to_mono(const uint8_t *p, const wav_fmt_t *fmt, int volume) {
  int bytes = fmt->bits / 8;
  int32_t sum = 0;
  for (int c = 0; c < fmt->channels; c++, p += bytes) {
    /* keep the top 16 bits; 8-bit samples are unsigned */
    sum += bytes == 1 ? ((int32_t)p[0] - 128) * 256 : (int16_t)le16(p + bytes - 2);
  }
  return (int16_t)(sum / fmt->channels * volume / 100);
}

static void buf_done(void *ctx, esp_err_t result) {
  wav_play_t *play = ctx;
  if (result != ESP_OK && play->error == ESP_OK) {
    play->error = result;
  }
  xSemaphoreGive(play->free_bufs);
}

esp_err_t wav_play(const char *path, int volume) {
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;

  FILE *f = fopen(path, "rb");
  if (!f) {
    return ESP_ERR_NOT_FOUND;
  }
  wav_fmt_t fmt = {0};
  uint32_t data_size = 0;
  esp_err_t ret = parse_header(f, &fmt, &data_size);
  if (ret == ESP_OK &&
      (fmt.format != WAV_FORMAT_PCM || fmt.channels == 0 || fmt.rate == 0 || fmt.bits % 8 != 0 ||
       fmt.bits < 8 || fmt.bits > 32 || fmt.block_align != fmt.channels * fmt.bits / 8)) {
    ESP_LOGE(TAG, "%s: format %u, %u ch, %u-bit not supported", path, fmt.format, fmt.channels, fmt.bits);
    ret = ESP_ERR_NOT_SUPPORTED;
  }
  if (ret != ESP_OK) {
    fclose(f);
    return ret;
  }
  ESP_LOGI(TAG, "%s: %lu Hz, %u ch, %u-bit, %lu bytes", path, (unsigned long)fmt.rate, fmt.channels, fmt.bits,
           (unsigned long)data_size);

  uint8_t *raw = malloc((size_t)WAV_BUF_FRAMES * fmt.block_align);
  int16_t *bufs = malloc(sizeof(int16_t) * WAV_BUFS * WAV_BUF_FRAMES);
  wav_play_t play = {.free_bufs = xSemaphoreCreateCounting(WAV_BUFS, WAV_BUFS), .error = ESP_OK};
  if (!raw || !bufs || !play.free_bufs) {
    ret = ESP_ERR_NO_MEM;
    goto out;
  }
  if ((ret = speaker_set_sample_rate(fmt.rate)) != ESP_OK) {
    goto out;
  }

  uint32_t left = data_size;
  for (int next = 0; left > 0 && play.error == ESP_OK; next = (next + 1) % WAV_BUFS) {
    size_t want = (size_t)WAV_BUF_FRAMES * fmt.block_align;
    if (want > left) want = left;
    size_t frames = fread(raw, 1, want, f) / fmt.block_align;
    if (frames == 0) {
      break; /* end of file (or a short last frame) */
    }
    left = data_size == 0xFFFFFFFF ? left : left - (uint32_t)(frames * fmt.block_align);

    xSemaphoreTake(play.free_bufs, portMAX_DELAY);
    int16_t *out = bufs + next * WAV_BUF_FRAMES;
    for (size_t i = 0; i < frames; i++) {
      out[i] = frame_to_mono(raw + i * fmt.block_align, &fmt, volume);
    }
    while ((ret = speaker_write(out, frames, buf_done, &play)) == ESP_ERR_NO_MEM) {
      vTaskDelay(pdMS_TO_TICKS(5)); /* speaker queue full (a key beep): retry */
    }
    if (ret != ESP_OK) {
      xSemaphoreGive(play.free_bufs);
      break;
    }
  }
  if (ret == ESP_OK) {
    ret = play.error;
  }
  if (ferror(f) && ret == ESP_OK) {
    ret = ESP_FAIL;
  }

  /* wait for the buffers still playing; they point into `bufs` */
  for (int i = 0; i < WAV_BUFS; i++) {
    xSemaphoreTake(play.free_bufs, portMAX_DELAY);
  }
  speaker_set_sample_rate(SPEAKER_SAMPLE_RATE);

out:
  if (play.free_bufs) {
    vSemaphoreDelete(play.free_bufs);
  }
  free(bufs);
  free(raw);
  fclose(f);
  return ret;
}
