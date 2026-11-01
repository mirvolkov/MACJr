/*
 * IR receiver HAL: decodes remote controls from a demodulating receiver
 * (TSOP-style, active low) on GPIO5.
 *
 * Port of Peter Hinch's MicroPython ir_rx (MIT license): an interrupt on
 * both edges records the time of each edge; the first edge opens a block
 * of IR_BLOCK_MS, and when it closes the recorded edges are decoded. The
 * block outlasts the longest frame (NEC, 67.5 ms) but closes before the
 * next frame or repeat code starts (NEC repeats every 108 ms). Only the
 * first frame of a block is decoded; the rest (Sony resends every 45 ms)
 * is dropped.
 *
 * Unlike ir_rx, which is built for one protocol at a time, the protocol
 * is told from the leader as its IR_GET does, and then decoded with the
 * rules of nec.py, sony.py and philips.py (RC-5). Only edges are timed,
 * so the receiver's polarity doesn't matter.
 */
#include "ir_rx.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "ir_rx";

#define IR_RX_GPIO    GPIO_NUM_5
#define IR_MAX_EDGES  100 /* per block, +1 recorded to see an overrun */
#define IR_BLOCK_MS   100
#define IR_GAP_US     10000 /* ends a burst (IR_GET) */
#define IR_QUEUE_LEN  8

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_times[IR_MAX_EDGES + 1]; /* us, written by the ISR */
static int s_edges;                         /* recorded in this block */
static TaskHandle_t s_task;
static QueueHandle_t s_queue;

static uint32_t s_burst[IR_MAX_EDGES]; /* last burst's durations, under s_lock */
static size_t s_burst_len;

static ir_event_t s_last_nec; /* for repeat codes */
static bool s_have_nec;

static void IRAM_ATTR edge_isr(void *arg) {
  uint32_t t = (uint32_t)esp_timer_get_time();
  bool first = false;
  portENTER_CRITICAL_ISR(&s_lock);
  /* On overrun ignore edges until the block closes */
  if (s_edges <= IR_MAX_EDGES) {
    first = s_edges == 0;
    s_times[s_edges++] = t;
  }
  portEXIT_CRITICAL_ISR(&s_lock);
  if (first) {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
  }
}

static bool near(uint32_t v, uint32_t target) {
  return v > target * 8 / 10 && v < target * 12 / 10;
}

/* Durations from `from` on until the first one over `max_us`: the frame
 * ends there. */
static int frame_len(const uint32_t *d, int n, int from, uint32_t max_us) {
  for (int i = from; i < n; i++) {
    if (d[i] > max_us) {
      return i;
    }
  }
  return n;
}

/* nec.py: data in the spaces, 1.69 ms (1) or 0.56 ms (0), LSB first. */
static bool decode_nec(const uint32_t *d, int n, ir_event_t *ev) {
  bool samsung = near(d[0], 4500) && near(d[1], 4500);
  if (!samsung && !near(d[0], 9000)) {
    return false;
  }
  if (!samsung && n >= 3 && near(d[1], 2250) && frame_len(d, n, 2, 2500) == 3) {
    if (!s_have_nec) {
      return false; /* repeat of a frame we missed */
    }
    *ev = s_last_nec;
    ev->repeat = true;
    ev->edges = 4;
    return true;
  }
  int m = frame_len(d, n, 2, 2500);
  if (m != 67 || !near(d[1], 4500)) {
    return false;
  }
  uint32_t val = 0;
  for (int i = 0; i < 32; i++) {
    if (d[3 + 2 * i] > 1120) {
      val |= 1u << i;
    }
  }
  uint8_t cmd = val >> 16;
  if ((cmd ^ (uint8_t)(val >> 24)) != 0xff) {
    return false; /* BADDATA */
  }
  uint8_t a = val, a2 = val >> 8;
  /* NEC sends the address inverted, Samsung twice; otherwise it is 16 bit */
  bool short_addr = samsung ? a == a2 : (a ^ a2) == 0xff;
  *ev = (ir_event_t){
    .proto = samsung ? IR_PROTO_SAMSUNG : IR_PROTO_NEC,
    .addr = short_addr ? a : (uint16_t)val,
    .cmd = cmd,
    .edges = m + 1,
  };
  if (!samsung) {
    s_last_nec = *ev;
    s_have_nec = true;
  }
  return true;
}

/* sony.py: data in the marks, 1.2 ms (1) or 0.6 ms (0), LSB first. */
static bool decode_sony(const uint32_t *d, int n, ir_event_t *ev) {
  if (!near(d[0], 2400) || !near(d[1], 600)) {
    return false;
  }
  int edges = frame_len(d, n, 1, 2000) + 1;
  if (edges != 26 && edges != 32 && edges != 42) {
    return false; /* BADBLOCK */
  }
  uint32_t val = 0, bit = 1;
  for (int x = 2; x <= edges - 2; x += 2) {
    if (d[x] > 900) {
      val |= bit;
    }
    bit <<= 1;
  }
  *ev = (ir_event_t){.proto = IR_PROTO_SONY, .cmd = val & 0x7f, .edges = edges};
  val >>= 7;
  if (edges < 42) {
    ev->addr = val & 0xff; /* 5 or 8 bit */
  } else {
    ev->addr = val & 0x1f;
    ev->ext = val >> 5;
  }
  return true;
}

/* philips.py RC5_IR: Manchester, 889 us half bits; regenerate the 14 bit
 * stream from the edges, MSB (a start bit, always 1) first. */
static bool decode_rc5(const uint32_t *d, int n, ir_event_t *ev) {
  if (!near(d[0], 889) && !near(d[0], 1778)) {
    return false;
  }
  int nedges = frame_len(d, n, 0, 2500) + 1;
  if (nedges < 14 || nedges > 28) {
    return false;
  }
  uint32_t v = 1;
  int bit = 1, x = 0;
  for (int bits = 1; bits < 14; bits++) {
    if (x > nedges - 2) {
      return false; /* BADBLOCK */
    }
    if (d[x] > 1200) { /* 2T: the data changes */
      bit ^= 1;
      x += 1;
    } else { /* T: skip the edge between two bits */
      x += 2;
    }
    v = (v << 1) | bit;
  }
  uint8_t cmd = v & 0x3f;
  if (!(v & 0x1000)) {
    cmd |= 0x40; /* field bit, inverted: commands 64-127 */
  }
  *ev = (ir_event_t){
    .proto = IR_PROTO_RC5,
    .addr = (v >> 6) & 0x1f,
    .cmd = cmd,
    .ext = (v >> 11) & 1,
    .edges = nedges,
  };
  return true;
}

/* Decode one block of `nedges` edge times. False for noise. */
static bool decode(const uint32_t *times, int nedges, ir_event_t *ev) {
  static uint32_t d[IR_MAX_EDGES];
  int n = nedges - 1;
  if (n < 3) {
    return false; /* noise */
  }
  for (int i = 0; i < n; i++) {
    d[i] = times[i + 1] - times[i];
  }

  int burst = frame_len(d, n, 1, IR_GAP_US);
  portENTER_CRITICAL(&s_lock);
  memcpy(s_burst, d, burst * sizeof(d[0]));
  s_burst_len = burst;
  portEXIT_CRITICAL(&s_lock);

  if (decode_nec(d, n, ev) || decode_sony(d, n, ev) || decode_rc5(d, n, ev)) {
    return true;
  }
  ESP_LOGD(TAG, "unknown burst: %d edges, start %lu %lu", burst + 1, (unsigned long)d[0],
           (unsigned long)d[1]);
  *ev = (ir_event_t){.proto = IR_PROTO_UNKNOWN, .edges = burst + 1};
  return true;
}

static void ir_task(void *arg) {
  static uint32_t times[IR_MAX_EDGES + 1];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* first edge of a block */
    vTaskDelay(pdMS_TO_TICKS(IR_BLOCK_MS));
    portENTER_CRITICAL(&s_lock);
    int nedges = s_edges;
    memcpy(times, s_times, nedges * sizeof(times[0]));
    s_edges = 0; /* the next edge opens a new block */
    portEXIT_CRITICAL(&s_lock);

    ir_event_t ev;
    if (!decode(times, nedges, &ev)) {
      continue;
    }
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
      ESP_LOGD(TAG, "queue full, event dropped");
    }
  }
}

esp_err_t ir_rx_init(void) {
  if (s_queue) {
    return ESP_ERR_INVALID_STATE;
  }
  s_queue = xQueueCreate(IR_QUEUE_LEN, sizeof(ir_event_t));
  if (!s_queue) {
    return ESP_ERR_NO_MEM;
  }
  if (xTaskCreate(ir_task, "ir_rx", 3072, NULL, 5, &s_task) != pdPASS) {
    vQueueDelete(s_queue);
    s_queue = NULL;
    return ESP_ERR_NO_MEM;
  }

  gpio_config_t io = {
    .pin_bit_mask = 1ULL << IR_RX_GPIO,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE, /* the receiver output idles high */
    .intr_type = GPIO_INTR_ANYEDGE,
  };
  esp_err_t err = gpio_config(&io);
  if (err == ESP_OK) {
    err = gpio_install_isr_service(0);
    if (err == ESP_ERR_INVALID_STATE) {
      err = ESP_OK; /* already installed by another driver */
    }
  }
  if (err == ESP_OK) {
    err = gpio_isr_handler_add(IR_RX_GPIO, edge_isr, NULL);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "GPIO%d setup failed: %s", IR_RX_GPIO, esp_err_to_name(err));
    return err; /* the task stays, idle: nothing will notify it */
  }
  ESP_LOGI(TAG, "receiving on GPIO%d", IR_RX_GPIO);
  return ESP_OK;
}

esp_err_t ir_rx_read(ir_event_t *ev, uint32_t timeout_ms) {
  if (!s_queue) {
    return ESP_ERR_INVALID_STATE;
  }
  TickType_t ticks = timeout_ms == IR_WAIT_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xQueueReceive(s_queue, ev, ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

size_t ir_rx_last_burst(uint32_t *us, size_t max) {
  portENTER_CRITICAL(&s_lock);
  size_t len = s_burst_len;
  memcpy(us, s_burst, (len < max ? len : max) * sizeof(us[0]));
  portEXIT_CRITICAL(&s_lock);
  return len;
}

const char *ir_proto_name(ir_proto_t proto) {
  switch (proto) {
  case IR_PROTO_NEC:
    return "NEC";
  case IR_PROTO_SAMSUNG:
    return "Samsung";
  case IR_PROTO_SONY:
    return "Sony";
  case IR_PROTO_RC5:
    return "RC-5";
  default:
    return "unknown";
  }
}
