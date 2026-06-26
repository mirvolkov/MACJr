/*
 * Host (non-ESP_PLATFORM) stand-in for the tiny slice of FreeRTOS that
 * tty_internal.h/.c and ldisc_ntty.c use: two mutex-style semaphores, one
 * binary "line ready" semaphore, and a fixed-capacity byte queue. Backed by
 * pthreads so the same tty.c/ldisc_ntty.c source builds unmodified on macOS.
 *
 * Only the exact calls used elsewhere in this component are implemented:
 * xSemaphoreCreate{Mutex,Binary}, xSemaphoreTake/Give, vSemaphoreDelete,
 * xQueueCreate/Send/Receive, vQueueDelete, with timeouts of either 0
 * (non-blocking) or portMAX_DELAY (block forever) -- the only two values
 * the real code passes.
 */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY ((TickType_t)UINT32_MAX)

/* Binary semaphore: count is 0 or 1. Also used as a plain mutex (take to
 * lock, give to unlock) -- functionally equivalent for this component's
 * strictly-paired take/give usage. */
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int count;
} macjr_host_sem_t;

typedef macjr_host_sem_t *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
  macjr_host_sem_t *s = (macjr_host_sem_t *)malloc(sizeof(*s));
  if (!s) {
    return NULL;
  }
  pthread_mutex_init(&s->mutex, NULL);
  pthread_cond_init(&s->cond, NULL);
  s->count = 1; /* unlocked */
  return s;
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) {
  macjr_host_sem_t *s = (macjr_host_sem_t *)malloc(sizeof(*s));
  if (!s) {
    return NULL;
  }
  pthread_mutex_init(&s->mutex, NULL);
  pthread_cond_init(&s->cond, NULL);
  s->count = 0; /* starts empty, unlike a mutex */
  return s;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
  pthread_mutex_lock(&s->mutex);
  if (ticks == 0) {
    if (s->count <= 0) {
      pthread_mutex_unlock(&s->mutex);
      return pdFALSE;
    }
  } else {
    while (s->count <= 0) {
      pthread_cond_wait(&s->cond, &s->mutex);
    }
  }
  s->count--;
  pthread_mutex_unlock(&s->mutex);
  return pdTRUE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
  pthread_mutex_lock(&s->mutex);
  s->count++;
  pthread_cond_signal(&s->cond);
  pthread_mutex_unlock(&s->mutex);
  return pdTRUE;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t s) {
  if (!s) {
    return;
  }
  pthread_mutex_destroy(&s->mutex);
  pthread_cond_destroy(&s->cond);
  free(s);
}

/* Fixed-capacity ring buffer of fixed-size items (only uint8_t items are
 * used in this component, but item_size is kept general). */
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
  uint8_t *buf;
  size_t item_size;
  size_t cap;
  size_t head;
  size_t count;
} macjr_host_queue_t;

typedef macjr_host_queue_t *QueueHandle_t;

static inline QueueHandle_t xQueueCreate(size_t cap, size_t item_size) {
  macjr_host_queue_t *q = (macjr_host_queue_t *)malloc(sizeof(*q));
  if (!q) {
    return NULL;
  }
  q->buf = (uint8_t *)malloc(cap * item_size);
  if (!q->buf) {
    free(q);
    return NULL;
  }
  pthread_mutex_init(&q->mutex, NULL);
  pthread_cond_init(&q->not_empty, NULL);
  pthread_cond_init(&q->not_full, NULL);
  q->item_size = item_size;
  q->cap = cap;
  q->head = 0;
  q->count = 0;
  return q;
}

static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks) {
  pthread_mutex_lock(&q->mutex);
  if (ticks == 0) {
    if (q->count >= q->cap) {
      pthread_mutex_unlock(&q->mutex);
      return pdFALSE;
    }
  } else {
    while (q->count >= q->cap) {
      pthread_cond_wait(&q->not_full, &q->mutex);
    }
  }
  size_t tail = (q->head + q->count) % q->cap;
  memcpy(q->buf + tail * q->item_size, item, q->item_size);
  q->count++;
  pthread_cond_signal(&q->not_empty);
  pthread_mutex_unlock(&q->mutex);
  return pdTRUE;
}

static inline BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t ticks) {
  pthread_mutex_lock(&q->mutex);
  if (ticks == 0) {
    if (q->count == 0) {
      pthread_mutex_unlock(&q->mutex);
      return pdFALSE;
    }
  } else {
    while (q->count == 0) {
      pthread_cond_wait(&q->not_empty, &q->mutex);
    }
  }
  memcpy(out, q->buf + q->head * q->item_size, q->item_size);
  q->head = (q->head + 1) % q->cap;
  q->count--;
  pthread_cond_signal(&q->not_full);
  pthread_mutex_unlock(&q->mutex);
  return pdTRUE;
}

static inline void vQueueDelete(QueueHandle_t q) {
  if (!q) {
    return;
  }
  pthread_mutex_destroy(&q->mutex);
  pthread_cond_destroy(&q->not_empty);
  pthread_cond_destroy(&q->not_full);
  free(q->buf);
  free(q);
}
