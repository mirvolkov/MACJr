/*
 * Updates over Wi-Fi (netupd.h): esp_http_server with a handful of
 * handlers. A firmware is streamed straight into the app slot that does
 * not run (as ota.c does from the card): nothing is stored on the way, and
 * the running firmware is untouched until the last step. A program is
 * written next to its final name (NAME.TMP) and renamed over it once
 * complete, so an interrupted upload leaves the old one.
 *
 * Writing flash pauses the cache for a moment at a time; a program that
 * runs meanwhile just stalls. tools/deploy.py sends the firmware first and
 * the programs after the restart, since programs bind to the firmware's
 * exports.
 */
#include "netupd.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "exec.h"

static const char *TAG = "netupd";

#define CHUNK        4096
#define NAME_MAX_LEN 40
#define TOKEN_MAX    64
#define RESTART_MS   500 /* after the reply has gone out */

static httpd_handle_t s_server;

/* Set before a restart this service asked for: survives the reset (RTC
 * memory, not cleared at boot), read once by netupd_rejoin(). */
#define REJOIN_MAGIC 0x4E555044u /* "NUPD" */
static RTC_NOINIT_ATTR uint32_t s_rejoin;

bool netupd_rejoin(void) {
  bool r = s_rejoin == REJOIN_MAGIC && esp_reset_reason() == ESP_RST_SW;
  s_rejoin = 0;
  return r;
}

/* ---- helpers ------------------------------------------------------------ */

static bool authorized(httpd_req_t *req) {
  char want[TOKEN_MAX + 1], got[TOKEN_MAX + 1];

  if (db_read(NETUPD_DB_KEY_TOKEN, want, sizeof(want)) != ESP_OK || !want[0]) {
    return true; /* no token set: open */
  }
  if (httpd_req_get_hdr_value_str(req, "X-Token", got, sizeof(got)) == ESP_OK && strcmp(got, want) == 0) {
    return true;
  }
  httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "bad or missing X-Token");
  return false;
}

/* Receive exactly `len` bytes (or what is left of the body). */
static int recv_full(httpd_req_t *req, char *buf, int len) {
  int got = 0;

  while (got < len) {
    int n = httpd_req_recv(req, buf + got, len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) {
      continue;
    }
    if (n <= 0) {
      return got ? got : -1;
    }
    got += n;
  }
  return got;
}

/* The program name in "/bin/name": letters, digits and ._- only, made
 * lower case. */
static bool bin_name(httpd_req_t *req, char *name) {
  const char *p = req->uri + strlen("/bin/");
  size_t n = strcspn(p, "?");

  if (n == 0 || n > NAME_MAX_LEN) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (!isalnum((unsigned char) p[i]) && p[i] != '.' && p[i] != '_' && p[i] != '-') {
      return false;
    }
  }
  for (size_t i = 0; i < n; i++) {
    name[i] = (char) tolower((unsigned char) p[i]); /* programs are lower case */
  }
  name[n] = '\0';
  return name[0] != '.';
}

static uint32_t file_crc(const char *path, long *size) {
  uint32_t crc = 0;
  char *buf = malloc(CHUNK);
  FILE *f = fopen(path, "rb");

  *size = 0;
  if (f && buf) {
    size_t n;
    while ((n = fread(buf, 1, CHUNK, f)) > 0) {
      crc = esp_rom_crc32_le(crc, (const uint8_t *) buf, n);
      *size += (long) n;
    }
  }
  if (f) {
    fclose(f);
  }
  free(buf);
  return crc;
}

static void restart_cb(void *arg) {
  esp_restart();
}

static void restart_soon(void) {
  const esp_timer_create_args_t args = {.callback = restart_cb, .name = "netupd_rst"};
  esp_timer_handle_t t;

  s_rejoin = REJOIN_MAGIC;
  if (esp_timer_create(&args, &t) == ESP_OK) {
    esp_timer_start_once(t, RESTART_MS * 1000);
  }
}

/* ---- GET /info ----------------------------------------------------------- */

static esp_err_t info_get(httpd_req_t *req) {
  if (!authorized(req)) {
    return ESP_OK;
  }
  const esp_app_desc_t *me = esp_app_get_description();
  const esp_partition_t *run = esp_ota_get_running_partition();
  char sha[65], line[400]; /* a file name can be 255 bytes */
  uint64_t total = 0, free_bytes = 0;

  for (int i = 0; i < 32; i++) {
    snprintf(sha + 2 * i, 3, "%02x", me->app_elf_sha256[i]);
  }
  exec_apps_info(&total, &free_bytes);
  httpd_resp_set_type(req, "application/json");
  snprintf(line, sizeof(line),
           "{\"project\":\"%s\",\"version\":\"%s\",\"slot\":\"%s\",\"sha256\":\"%s\",\"apps_total\":%llu,"
           "\"apps_free\":%llu,\"bin\":[",
           me->project_name, me->version, run->label, sha, (unsigned long long) total,
           (unsigned long long) free_bytes);
  httpd_resp_sendstr_chunk(req, line);

  DIR *dir = opendir(EXEC_BIN_DIR);
  bool first = true;
  if (dir) {
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
      char path[sizeof(EXEC_BIN_DIR) + NAME_MAX_LEN + 260];
      struct stat st;
      long size;

      snprintf(path, sizeof(path), "%s/%s", EXEC_BIN_DIR, de->d_name);
      if (de->d_name[0] == '.' || stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        continue;
      }
      uint32_t crc = file_crc(path, &size);
      snprintf(line, sizeof(line), "%s{\"name\":\"%s\",\"size\":%ld,\"crc32\":\"%08lx\"}", first ? "" : ",",
               de->d_name, size, (unsigned long) crc);
      httpd_resp_sendstr_chunk(req, line);
      first = false;
    }
    closedir(dir);
  }
  httpd_resp_sendstr_chunk(req, "]}\n");
  return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---- PUT /ota ------------------------------------------------------------ */

static esp_err_t ota_put(httpd_req_t *req) {
  if (!authorized(req)) {
    return ESP_OK;
  }
  const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
  const esp_app_desc_t *me = esp_app_get_description();
  size_t total = req->content_len;
  const size_t head = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);

  if (!slot || total < head || total > slot->size) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no slot, or bad size");
  }
  char *buf = malloc(CHUNK);
  if (!buf) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
  }
  /* the first chunk holds the app description: check it before erasing */
  int n = recv_full(req, buf, total < CHUNK ? (int) total : CHUNK);
  const esp_image_header_t *hdr = (const esp_image_header_t *) buf;
  const esp_app_desc_t *desc =
      (const esp_app_desc_t *) (buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
  if (n < (int) head || hdr->magic != ESP_IMAGE_HEADER_MAGIC || hdr->chip_id != ESP_CHIP_ID_ESP32S3 ||
      desc->magic_word != ESP_APP_DESC_MAGIC_WORD ||
      strncmp(desc->project_name, me->project_name, sizeof(desc->project_name)) != 0) {
    free(buf);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not a MacJr firmware");
  }
  char version[sizeof(desc->version) + 1];
  snprintf(version, sizeof(version), "%s", desc->version);
  ESP_LOGI(TAG, "firmware %s (%u bytes) -> %s", version, (unsigned) total, slot->label);

  esp_ota_handle_t h;
  esp_err_t err = esp_ota_begin(slot, total, &h);
  size_t done = 0;
  while (err == ESP_OK && n > 0) {
    err = esp_ota_write(h, buf, n);
    done += n;
    if (done >= total) {
      break;
    }
    n = recv_full(req, buf, total - done < CHUNK ? (int) (total - done) : CHUNK);
  }
  free(buf);
  if (err == ESP_OK && done != total) {
    err = ESP_ERR_INVALID_SIZE; /* the connection broke */
  }
  if (err == ESP_OK) {
    err = esp_ota_end(h); /* checks the image and its SHA-256 */
  } else {
    esp_ota_abort(h);
  }
  if (err == ESP_OK) {
    err = esp_ota_set_boot_partition(slot);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "firmware update failed: %s", esp_err_to_name(err));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
  }

  char q[32], v[8];
  bool reboot = true;
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
      httpd_query_key_value(q, "reboot", v, sizeof(v)) == ESP_OK && strcmp(v, "0") == 0) {
    reboot = false;
  }
  char reply[96];
  snprintf(reply, sizeof(reply), "ok: %s in %s%s\n", version, slot->label, reboot ? ", restarting" : "");
  httpd_resp_sendstr(req, reply);
  ESP_LOGI(TAG, "%s", reply);
  if (reboot) {
    restart_soon();
  }
  return ESP_OK;
}

/* ---- /bin/NAME ------------------------------------------------------------ */

static esp_err_t bin_put(httpd_req_t *req) {
  if (!authorized(req)) {
    return ESP_OK;
  }
  char name[NAME_MAX_LEN + 1], path[96], tmp[100];
  if (!bin_name(req, name)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
  }
  snprintf(path, sizeof(path), "%s/%s", EXEC_BIN_DIR, name);
  snprintf(tmp, sizeof(tmp), "%s.TMP", path);

  FILE *f = fopen(tmp, "wb");
  char *buf = malloc(CHUNK);
  if (!f || !buf) {
    if (f) {
      fclose(f);
    }
    free(buf);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot write");
  }
  size_t total = req->content_len, done = 0;
  uint32_t crc = 0;
  bool ok = true;
  while (done < total) {
    int n = recv_full(req, buf, total - done < CHUNK ? (int) (total - done) : CHUNK);
    if (n <= 0 || fwrite(buf, 1, n, f) != (size_t) n) {
      ok = false;
      break;
    }
    crc = esp_rom_crc32_le(crc, (const uint8_t *) buf, n);
    done += n;
  }
  free(buf);
  if (fclose(f) != 0) {
    ok = false;
  }
  if (!ok) {
    unlink(tmp);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed (full?)");
  }
  unlink(path);
  if (rename(tmp, path) != 0) {
    unlink(tmp);
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
  }
  char reply[96];
  snprintf(reply, sizeof(reply), "ok: %s %u bytes crc32 %08lx\n", name, (unsigned) done, (unsigned long) crc);
  ESP_LOGI(TAG, "%s", reply);
  return httpd_resp_sendstr(req, reply);
}

static esp_err_t bin_delete(httpd_req_t *req) {
  if (!authorized(req)) {
    return ESP_OK;
  }
  char name[NAME_MAX_LEN + 1], path[96];
  if (!bin_name(req, name)) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
  }
  snprintf(path, sizeof(path), "%s/%s", EXEC_BIN_DIR, name);
  if (unlink(path) != 0) {
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such program");
  }
  ESP_LOGI(TAG, "deleted %s", name);
  return httpd_resp_sendstr(req, "ok\n");
}

static esp_err_t reboot_post(httpd_req_t *req) {
  if (!authorized(req)) {
    return ESP_OK;
  }
  httpd_resp_sendstr(req, "ok: restarting\n");
  restart_soon();
  return ESP_OK;
}

/* ---- start / stop -------------------------------------------------------- */

esp_err_t netupd_start(void) {
  if (s_server) {
    return ESP_OK;
  }
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = NETUPD_PORT;
  cfg.stack_size = 8192;
  cfg.core_id = 0;               /* with the Wi-Fi; programs run on core 1 */
  cfg.recv_wait_timeout = 30;    /* flash erases can hold a write for a while */
  cfg.send_wait_timeout = 30;
  cfg.lru_purge_enable = true;
  cfg.uri_match_fn = httpd_uri_match_wildcard;

  esp_err_t err = httpd_start(&s_server, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
    s_server = NULL;
    return err;
  }
  static const httpd_uri_t uris[] = {
      {.uri = "/info", .method = HTTP_GET, .handler = info_get},
      {.uri = "/ota", .method = HTTP_PUT, .handler = ota_put},
      {.uri = "/bin/*", .method = HTTP_PUT, .handler = bin_put},
      {.uri = "/bin/*", .method = HTTP_DELETE, .handler = bin_delete},
      {.uri = "/reboot", .method = HTTP_POST, .handler = reboot_post},
  };
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
    httpd_register_uri_handler(s_server, &uris[i]);
  }
  ESP_LOGI(TAG, "update service on port %d", NETUPD_PORT);
  return ESP_OK;
}

void netupd_stop(void) {
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "update service stopped");
  }
}
