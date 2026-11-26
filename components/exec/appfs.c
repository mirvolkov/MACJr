/*
 * The programs' own flash partition ("apps" in partitions.csv): a FAT file
 * system with wear levelling, mounted as EXEC_BIN_DIR (/bin), the
 * programs in its root. The network updater (main/netupd.c) and
 * apps/tools/flashapps.py (an image of the whole partition, over USB)
 * write it.
 */
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "exec.h"
#include "wear_levelling.h"

static const char *TAG = "appfs";

#define APPS_PARTITION "apps"

static wl_handle_t s_wl = WL_INVALID_HANDLE;

esp_err_t exec_apps_init(void) {
  const esp_vfs_fat_mount_config_t cfg = {
      .format_if_mount_failed = true, /* a new partition comes up empty */
      .max_files = 6,
      .allocation_unit_size = 4096,
  };
  esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(EXEC_BIN_DIR, APPS_PARTITION, &cfg, &s_wl);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mount %s: %s", EXEC_BIN_DIR, esp_err_to_name(err));
    return err;
  }
  uint64_t total = 0, free_bytes = 0;
  esp_vfs_fat_info(EXEC_BIN_DIR, &total, &free_bytes);
  ESP_LOGI(TAG, "%s: %llu KB, %llu KB free", EXEC_BIN_DIR, (unsigned long long)(total / 1024),
           (unsigned long long)(free_bytes / 1024));
  return ESP_OK;
}

esp_err_t exec_apps_info(uint64_t *total, uint64_t *free_bytes) {
  return esp_vfs_fat_info(EXEC_BIN_DIR, total, free_bytes);
}
