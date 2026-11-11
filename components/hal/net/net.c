/*
 * Wi-Fi station (see net.h). Connection progress comes from the default
 * event loop: the handler records the outcome in an event group, which
 * net_wifi_connect() waits on. Wi-Fi doesn't reconnect on its own: a
 * dropped connection stays dropped until the next net_wifi_connect().
 * The first connection also starts SNTP, which sets the system clock.
 */
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

#define BIT_GOT_IP BIT0
#define BIT_DOWN   BIT1 /* disconnected, or the connect attempt failed */

#define DISCONNECT_WAIT_MS 2000
#define SCAN_RECORDS_MAX   32 /* records read from the driver */

static const char *TAG = "net";

static SemaphoreHandle_t s_lock; /* one call at a time */
static StaticSemaphore_t s_lock_buf;
static EventGroupHandle_t s_events;
static StaticEventGroup_t s_events_buf;
static bool s_up;           /* Wi-Fi started */
static bool s_connected;    /* associated, with or without an address */
static uint8_t s_reason;    /* of the last disconnect */
static char s_ssid[NET_SSID_MAX + 1];
static char s_ip[16];
static bool s_sntp;         /* SNTP started */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    s_connected = true;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const wifi_event_sta_disconnected_t *ev = data;
    s_connected = false;
    s_reason = ev->reason;
    s_ip[0] = '\0';
    xEventGroupClearBits(s_events, BIT_GOT_IP);
    xEventGroupSetBits(s_events, BIT_DOWN);
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *ev = data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    xEventGroupSetBits(s_events, BIT_GOT_IP);
  }
}

static esp_err_t start_nvs(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS partition erased: %s", esp_err_to_name(err));
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  return err;
}

/* Bring Wi-Fi up on first use. Under s_lock. */
static esp_err_t start(void) {
  if (s_up) {
    return ESP_OK;
  }
  esp_err_t err = start_nvs();
  if (err == ESP_OK) {
    err = esp_netif_init();
  }
  if (err == ESP_OK) {
    err = esp_event_loop_create_default();
    if (err == ESP_ERR_INVALID_STATE) {
      err = ESP_OK; /* someone created it already */
    }
  }
  if (err == ESP_OK && !esp_netif_create_default_wifi_sta()) {
    err = ESP_FAIL;
  }
  if (err == ESP_OK) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
  }
  if (err == ESP_OK) {
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    err = esp_wifi_set_mode(WIFI_MODE_STA);
  }
  if (err == ESP_OK) {
    err = esp_wifi_start();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
    return err;
  }
  s_up = true;
  ESP_LOGI(TAG, "Wi-Fi station started");
  return ESP_OK;
}

/* Take the lock, creating it on first use, and start Wi-Fi. */
static esp_err_t enter(void) {
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&mux);
  if (!s_lock) {
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    s_events = xEventGroupCreateStatic(&s_events_buf);
  }
  portEXIT_CRITICAL(&mux);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  esp_err_t err = start();
  if (err != ESP_OK) {
    xSemaphoreGive(s_lock);
  }
  return err;
}

static void leave(void) {
  xSemaphoreGive(s_lock);
}

/* Stop the current connection or attempt and wait for the driver to
 * report it, so its event doesn't land on the next attempt. Under s_lock. */
static void stop(void) {
  xEventGroupClearBits(s_events, BIT_DOWN);
  esp_wifi_disconnect();
  xEventGroupWaitBits(s_events, BIT_DOWN, pdFALSE, pdFALSE, pdMS_TO_TICKS(DISCONNECT_WAIT_MS));
}

/* Drop the connection, if there is one. Under s_lock. */
static void disconnect(void) {
  if (s_connected) {
    stop();
  }
}

static bool known(const net_ap_t *aps, int n, const char *ssid) {
  for (int i = 0; i < n; i++) {
    if (strcmp(aps[i].ssid, ssid) == 0) {
      return true;
    }
  }
  return false;
}

int net_wifi_scan(net_ap_t *aps, int max) {
  if (enter() != ESP_OK) {
    return -1;
  }
  int n = -1;
  wifi_ap_record_t *records = malloc(SCAN_RECORDS_MAX * sizeof(*records));
  esp_err_t err = records ? esp_wifi_scan_start(NULL, true) : ESP_ERR_NO_MEM;
  if (err == ESP_OK) {
    uint16_t count = SCAN_RECORDS_MAX;
    err = esp_wifi_scan_get_ap_records(&count, records);
    n = 0;
    /* The driver lists them strongest first: keep the first of each name. */
    for (int i = 0; err == ESP_OK && i < count && n < max; i++) {
      const char *ssid = (const char *)records[i].ssid;
      if (!ssid[0] || known(aps, n, ssid)) {
        continue;
      }
      snprintf(aps[n].ssid, sizeof(aps[n].ssid), "%s", ssid);
      aps[n].rssi = records[i].rssi;
      aps[n].open = records[i].authmode == WIFI_AUTH_OPEN;
      n++;
    }
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "scan failed: %s", esp_err_to_name(err));
    esp_wifi_clear_ap_list();
    n = -1;
  }
  free(records);
  leave();
  return n;
}

/* What a disconnect reason means for net_wifi_connect(). */
static esp_err_t reason_err(uint8_t reason) {
  switch (reason) {
  case WIFI_REASON_AUTH_EXPIRE:
  case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
  case WIFI_REASON_AUTH_FAIL:
  case WIFI_REASON_HANDSHAKE_TIMEOUT:
    return ESP_ERR_WIFI_PASSWORD;
  case WIFI_REASON_NO_AP_FOUND:
  case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
  case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
  case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
    return ESP_ERR_WIFI_SSID;
  default:
    return ESP_FAIL;
  }
}

/* Keep the system clock set over NTP from the first connection on. SNTP
 * stays running across reconnects and syncs again every hour. Under s_lock. */
static void start_sntp(void) {
  if (s_sntp) {
    return;
  }
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NET_NTP_SERVER);
  esp_err_t err = esp_netif_sntp_init(&cfg);
  if (err == ESP_OK) {
    s_sntp = true;
    ESP_LOGI(TAG, "SNTP started (%s)", NET_NTP_SERVER);
  } else {
    ESP_LOGW(TAG, "SNTP start failed: %s", esp_err_to_name(err));
  }
}

esp_err_t net_wifi_connect(const char *ssid, const char *pass, uint32_t timeout_ms) {
  if (!ssid || !*ssid || strlen(ssid) > NET_SSID_MAX || !pass || strlen(pass) > NET_PASS_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t err = enter();
  if (err != ESP_OK) {
    return err;
  }
  disconnect();
  s_ssid[0] = '\0';

  wifi_config_t cfg = {0};
  memcpy(cfg.sta.ssid, ssid, strlen(ssid));
  memcpy(cfg.sta.password, pass, strlen(pass));
  cfg.sta.threshold.authmode = *pass ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
  err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
  if (err == ESP_OK) {
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_DOWN);
    err = esp_wifi_connect();
  }
  if (err == ESP_OK) {
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_DOWN, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_GOT_IP) {
      snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
      ESP_LOGI(TAG, "connected to %s, %s", s_ssid, s_ip);
      start_sntp();
    } else if (bits & BIT_DOWN) {
      err = reason_err(s_reason);
      ESP_LOGW(TAG, "connecting to %s failed: reason %u", ssid, s_reason);
    } else {
      err = ESP_ERR_TIMEOUT;
      stop();
      ESP_LOGW(TAG, "connecting to %s timed out", ssid);
    }
  }
  leave();
  return err;
}

/* Start Wi-Fi (STA) without connecting, so the radio/PHY is up. Idempotent.
 * The SDR capture HAL (espsdr) needs the PHY running. */
esp_err_t net_wifi_up(void) {
  esp_err_t err = enter();
  if (err == ESP_OK) {
    leave();
  }
  return err;
}

void net_wifi_disconnect(void) {
  if (enter() != ESP_OK) {
    return;
  }
  disconnect();
  s_ssid[0] = '\0';
  leave();
}

/* The station MAC (from eFuse; works even when Wi-Fi is down). */
void net_wifi_mac(uint8_t mac[6]) {
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

bool net_wifi_status(char *ssid, char *ip) {
  if (!s_up || !s_events || !(xEventGroupGetBits(s_events) & BIT_GOT_IP)) {
    return false;
  }
  if (ssid) {
    strcpy(ssid, s_ssid);
  }
  if (ip) {
    strcpy(ip, s_ip);
  }
  return true;
}
