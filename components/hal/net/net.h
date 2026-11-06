#ifndef MACJR_NET_H
#define MACJR_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h" /* ESP_ERR_WIFI_* */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wi-Fi station. The radio, the TCP/IP stack and NVS (where the PHY keeps
 * its calibration) are brought up by the first call that needs them, so
 * the firmware pays nothing for Wi-Fi until a program uses it. Wi-Fi then
 * stays on, connected or not, until power off. Calls are thread-safe and
 * block; the Wi-Fi tasks run on CPU0.
 *
 * Credentials are not stored here: Wi-Fi keeps its configuration in RAM
 * only, and saving a network is up to the caller (the WIFI program keeps
 * NET_DB_KEY_SSID / NET_DB_KEY_PASS in the db).
 */
#define NET_SSID_MAX 32 /* bytes, without the terminator */
#define NET_PASS_MAX 64

#define NET_DB_KEY_SSID "wifi.ssid"
#define NET_DB_KEY_PASS "wifi.pass"

typedef struct {
  char ssid[NET_SSID_MAX + 1];
  int8_t rssi; /* dBm */
  bool open;   /* no password needed */
} net_ap_t;

/* Scan all channels (takes a couple of seconds) and fill `aps` with up to
 * `max` networks, strongest first, one entry per name; hidden networks
 * are left out. The number of entries, or -1 on failure. */
int net_wifi_scan(net_ap_t *aps, int max);

/* Connect to `ssid` with `pass` ("" for an open network), dropping the
 * current connection first, and wait up to `timeout_ms` for an IP
 * address. ESP_OK once there is one; ESP_ERR_WIFI_PASSWORD when the
 * network refused the password, ESP_ERR_WIFI_SSID when it wasn't found,
 * ESP_ERR_TIMEOUT, or another error. */
esp_err_t net_wifi_connect(const char *ssid, const char *pass, uint32_t timeout_ms);

/* Drop the connection, if any. */
void net_wifi_disconnect(void);

/* True while connected with an IP address; then the network's name goes
 * to `ssid` (NET_SSID_MAX + 1 bytes) and the address, as text, to `ip`
 * (16 bytes). Either may be NULL. */
bool net_wifi_status(char *ssid, char *ip);

#ifdef __cplusplus
}
#endif

#endif
