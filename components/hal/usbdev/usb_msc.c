/*
 * USB mass-storage callbacks: the host's SCSI READ(10) / WRITE(10) map
 * straight onto SD card sectors, so the host sees the card exactly as a
 * card reader would (partition table, FAT and all). Runs in the TinyUSB
 * task.
 */
#include <string.h>

#include "esp_log.h"
#include "tusb.h"
#include "usbdev_priv.h"

static const char *TAG = "usb_msc";

static sdmmc_card_t *s_card;
static volatile bool s_ejected;
static volatile uint64_t s_read;
static volatile uint64_t s_written;

void usbdev_msc_set_card(sdmmc_card_t *card) {
  s_card = card;
}

void usbdev_msc_status(usbdev_msc_status_t *out) {
  out->mounted = tud_mounted();
  out->ejected = s_ejected;
  out->bytes_read = s_read;
  out->bytes_written = s_written;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
  (void)lun;
  memcpy(vendor_id, "MACJr   ", 8);
  memcpy(product_id, "SD card         ", 16);
  memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  if (!s_card || s_ejected) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00); /* medium not present */
    return false;
  }
  return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
  (void)lun;
  *block_count = s_card ? (uint32_t)s_card->csd.capacity : 0;
  *block_size = s_card ? (uint16_t)s_card->csd.sector_size : 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
  (void)lun;
  (void)power_condition;
  if (load_eject) {
    s_ejected = !start;
    ESP_LOGI(TAG, "host %s the drive", start ? "loaded" : "ejected");
  }
  return true;
}

/* `offset` is the byte position inside the transfer that starts at `lba`;
 * with CFG_TUD_MSC_EP_BUFSIZE a multiple of 512 it always lands on a sector. */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  (void)lun;
  if (!s_card) {
    return -1;
  }
  size_t sector = s_card->csd.sector_size;
  if (offset % sector || bufsize % sector) {
    return -1;
  }
  if (sdmmc_read_sectors(s_card, buffer, lba + offset / sector, bufsize / sector) != ESP_OK) {
    return -1;
  }
  s_read += bufsize;
  return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
  (void)lun;
  if (!s_card) {
    return -1;
  }
  size_t sector = s_card->csd.sector_size;
  if (offset % sector || bufsize % sector) {
    return -1;
  }
  if (sdmmc_write_sectors(s_card, buffer, lba + offset / sector, bufsize / sector) != ESP_OK) {
    return -1;
  }
  s_written += bufsize;
  return (int32_t)bufsize;
}

/* SCSI commands TinyUSB does not handle itself: refuse them. */
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
  (void)scsi_cmd;
  (void)buffer;
  (void)bufsize;
  tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00); /* invalid command */
  return -1;
}
