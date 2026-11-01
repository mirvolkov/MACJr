/*
 * IR transmitter HAL: an IR LED on GPIO4, driven by an RMT TX channel.
 * A frame is a list of durations, mark first (LED blinking at the
 * carrier) then space (LED off), packed two per RMT symbol and sent with
 * the copy encoder; the RMT modulates the carrier onto the marks itself.
 *
 * The frames are the inverse of what ir_rx.c decodes, with the nominal
 * timings of each protocol.
 */
#include "ir_tx.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ir_tx";

#define IR_TX_GPIO        GPIO_NUM_4
#define IR_TX_RESOLUTION  1000000 /* 1 us ticks */
#define IR_TX_MAX_DUR     128
#define IR_TX_DONE_MS     1000
#define IR_TX_MAX_TICKS   32767 /* 15 bit symbol duration */

#define SONY_PERIOD_US    45000
#define SONY_FRAMES       3

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static SemaphoreHandle_t s_mutex;
static uint32_t s_carrier_hz; /* applied to s_chan */

static rmt_symbol_word_t s_sym[IR_TX_MAX_DUR / 2];

/* Frame being built: durations in us, mark first. */
typedef struct {
  uint32_t d[IR_TX_MAX_DUR];
  int n;
} frame_t;

static void add(frame_t *f, uint32_t mark, uint32_t space) {
  if (f->n + 2 <= IR_TX_MAX_DUR) {
    f->d[f->n++] = mark;
    f->d[f->n++] = space;
  }
}

static uint32_t total_us(const frame_t *f) {
  uint32_t t = 0;
  for (int i = 0; i < f->n; i++) {
    t += f->d[i];
  }
  return t;
}

/* 32 bits LSB first: 560 us mark, then 1690 us (1) or 560 us (0) space. */
static void nec_frame(frame_t *f, const ir_event_t *ev) {
  bool samsung = ev->proto == IR_PROTO_SAMSUNG;
  if (ev->repeat && !samsung) {
    add(f, 9000, 2250);
    add(f, 560, 0);
    return;
  }
  uint32_t addr = ev->addr;
  if (addr <= 0xff) {
    addr |= (samsung ? addr : addr ^ 0xff) << 8;
  }
  uint32_t val = (addr & 0xffff) | (uint32_t)ev->cmd << 16 | (uint32_t)(ev->cmd ^ 0xff) << 24;
  add(f, samsung ? 4500 : 9000, 4500);
  for (int i = 0; i < 32; i++) {
    add(f, 560, (val >> i) & 1 ? 1690 : 560);
  }
  add(f, 560, 0);
}

/* 2.4 ms leader, then bits LSB first as 1.2 ms (1) or 0.6 ms (0) marks
 * after 0.6 ms spaces; 12, 15 or 20 bits by the edges received. */
static void sony_frame(frame_t *f, const ir_event_t *ev) {
  int bits = ev->edges == 42 ? 20 : ev->edges == 32 ? 15 : 12;
  uint32_t val = ev->cmd & 0x7f;
  if (bits == 20) {
    val |= (uint32_t)(ev->addr & 0x1f) << 7 | (uint32_t)ev->ext << 12;
  } else {
    val |= (uint32_t)ev->addr << 7;
  }
  uint32_t mark = 2400;
  for (int i = 0; i < bits; i++) {
    add(f, mark, 600);
    mark = (val >> i) & 1 ? 1200 : 600;
  }
  add(f, mark, 0);
  /* the last space pads the frame to the 45 ms repeat period */
  uint32_t gap = SONY_PERIOD_US - total_us(f);
  f->d[f->n - 1] = gap < IR_TX_MAX_TICKS ? gap : IR_TX_MAX_TICKS;
}

/* 14 bits MSB first, Manchester with 889 us halves: 1 is space then
 * mark, 0 mark then space. Runs of one level become one duration; the
 * space before the first mark is the idle line. */
static void rc5_frame(frame_t *f, const ir_event_t *ev) {
  uint32_t v = 1u << 13 | (uint32_t)!(ev->cmd & 0x40) << 12 | (uint32_t)(ev->ext & 1) << 11 |
               (uint32_t)(ev->addr & 0x1f) << 6 | (ev->cmd & 0x3f);
  int level = 0;     /* 1: mark */
  uint32_t run = 0;  /* us at `level` so far */
  for (int i = 13; i >= 0; i--) {
    int bit = (v >> i) & 1;
    for (int half = 0; half < 2; half++) {
      int l = half ? bit : !bit;
      if (l != level) {
        if (level || f->n) { /* drop the idle space before the first mark */
          f->d[f->n++] = run;
        }
        level = l;
        run = 0;
      }
      run += 889;
    }
  }
  if (level) {
    f->d[f->n++] = run;
  }
}

/* Send the durations as RMT symbols, `times` frames back to back. */
static esp_err_t send(const uint32_t *d, int n, uint32_t carrier_hz, int times) {
  if (!s_chan) {
    return ESP_ERR_INVALID_STATE;
  }
  if (n <= 0 || n > IR_TX_MAX_DUR) {
    return ESP_ERR_INVALID_ARG;
  }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  esp_err_t err = ESP_OK;
  if (carrier_hz != s_carrier_hz) {
    rmt_carrier_config_t carrier = {.frequency_hz = carrier_hz, .duty_cycle = 0.33};
    rmt_disable(s_chan);
    err = rmt_apply_carrier(s_chan, &carrier);
    rmt_enable(s_chan);
    s_carrier_hz = err == ESP_OK ? carrier_hz : 0;
  }
  int nsym = (n + 1) / 2;
  for (int i = 0; i < nsym; i++) {
    uint32_t mark = d[2 * i];
    uint32_t space = 2 * i + 1 < n ? d[2 * i + 1] : 0; /* 0 ends the transmission */
    s_sym[i] = (rmt_symbol_word_t){
      .level0 = 1,
      .duration0 = mark < IR_TX_MAX_TICKS ? mark : IR_TX_MAX_TICKS,
      .level1 = 0,
      .duration1 = space < IR_TX_MAX_TICKS ? space : IR_TX_MAX_TICKS,
    };
  }
  rmt_transmit_config_t tx = {.loop_count = 0};
  for (int i = 0; i < times && err == ESP_OK; i++) {
    err = rmt_transmit(s_chan, s_enc, s_sym, nsym * sizeof(s_sym[0]), &tx);
  }
  if (err == ESP_OK) {
    err = rmt_tx_wait_all_done(s_chan, IR_TX_DONE_MS);
  }
  xSemaphoreGive(s_mutex);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "send failed: %s", esp_err_to_name(err));
  }
  return err;
}

esp_err_t ir_tx_init(void) {
  if (s_chan) {
    return ESP_ERR_INVALID_STATE;
  }
  s_mutex = xSemaphoreCreateMutex();
  if (!s_mutex) {
    return ESP_ERR_NO_MEM;
  }
  rmt_tx_channel_config_t cfg = {
    .gpio_num = IR_TX_GPIO,
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = IR_TX_RESOLUTION,
    .mem_block_symbols = 48,
    .trans_queue_depth = SONY_FRAMES,
  };
  rmt_copy_encoder_config_t enc = {};
  rmt_channel_handle_t chan = NULL;
  esp_err_t err = rmt_new_tx_channel(&cfg, &chan);
  if (err == ESP_OK) {
    err = rmt_new_copy_encoder(&enc, &s_enc);
  }
  if (err == ESP_OK) {
    rmt_carrier_config_t carrier = {.frequency_hz = IR_TX_CARRIER_HZ, .duty_cycle = 0.33};
    err = rmt_apply_carrier(chan, &carrier);
  }
  if (err == ESP_OK) {
    err = rmt_enable(chan);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "RMT on GPIO%d failed: %s", IR_TX_GPIO, esp_err_to_name(err));
    if (s_enc) {
      rmt_del_encoder(s_enc);
      s_enc = NULL;
    }
    if (chan) {
      rmt_del_channel(chan);
    }
    return err;
  }
  s_carrier_hz = IR_TX_CARRIER_HZ;
  s_chan = chan;
  ESP_LOGI(TAG, "sending on GPIO%d", IR_TX_GPIO);
  return ESP_OK;
}

esp_err_t ir_tx_send(const ir_event_t *ev) {
  frame_t local = {0};
  switch (ev->proto) {
  case IR_PROTO_NEC:
  case IR_PROTO_SAMSUNG:
    nec_frame(&local, ev);
    return send(local.d, local.n, 38000, 1);
  case IR_PROTO_SONY:
    sony_frame(&local, ev);
    return send(local.d, local.n, 40000, SONY_FRAMES);
  case IR_PROTO_RC5:
    rc5_frame(&local, ev);
    return send(local.d, local.n, 36000, 1);
  default:
    return ESP_ERR_NOT_SUPPORTED;
  }
}

esp_err_t ir_tx_send_raw(const uint32_t *us, size_t n, uint32_t carrier_hz) {
  return send(us, (int)n, carrier_hz, 1);
}
