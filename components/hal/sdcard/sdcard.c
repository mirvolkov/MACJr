/*
 * SD-over-SPI HAL: mounts a FAT-formatted SD card at SDCARD_MOUNT_POINT so
 * the rest of the firmware can use plain stdio (fopen/opendir) against it.
 */
#include "sdcard.h"

#include <stdbool.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

#define SD_SCK  12
#define SD_MISO 13
#define SD_MOSI 11
#define SD_CS   10
#define SD_HOST SPI2_HOST

static sdmmc_card_t *s_card;    /* FAT-mounted card (sdcard_init) */
static sdmmc_card_t s_raw_card;  /* raw card without a filesystem (sdcard_open_raw) */
static bool s_raw_open;

static const sdspi_device_config_t s_slot_config = {
    .gpio_cs = SD_CS,
    .host_id = SD_HOST,
    .gpio_cd = SDSPI_SLOT_NO_CD,
    .gpio_wp = SDSPI_SLOT_NO_WP,
    .gpio_int = SDSPI_SLOT_NO_INT,
};

static esp_err_t bus_init(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(SD_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t sdcard_init(void)
{
    ESP_LOGI(TAG, "Initializing SD card");

    esp_err_t ret = bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host_config = SDSPI_HOST_DEFAULT();

    ret = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host_config, &s_slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.",
                     esp_err_to_name(ret));
        }
        spi_bus_free(SD_HOST);
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t sdcard_info(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_card) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_vfs_fat_info(SDCARD_MOUNT_POINT, total_bytes, free_bytes);
}

esp_err_t sdcard_open_raw(sdmmc_card_t **out)
{
    if (s_raw_open) {
        *out = &s_raw_card;
        return ESP_OK;
    }
    sdcard_deinit(); /* nobody may use the filesystem while sectors change underneath */

    esp_err_t ret = bus_init();
    if (ret != ESP_OK) {
        return ret;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_dev_handle_t dev;
    ret = sdspi_host_init();
    if (ret == ESP_OK) {
        ret = sdspi_host_init_device(&s_slot_config, &dev);
    }
    if (ret == ESP_OK) {
        host.slot = dev;
        ret = sdmmc_card_init(&host, &s_raw_card);
        if (ret != ESP_OK) {
            sdspi_host_remove_device(dev);
        }
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "raw card init failed: %s", esp_err_to_name(ret));
        spi_bus_free(SD_HOST);
        return ret;
    }
    s_raw_open = true;
    *out = &s_raw_card;
    ESP_LOGI(TAG, "Card open for raw sector access, FAT unmounted");
    return ESP_OK;
}

esp_err_t sdcard_close_raw(void)
{
    if (s_raw_open) {
        sdspi_host_remove_device(s_raw_card.host.slot);
        spi_bus_free(SD_HOST);
        s_raw_open = false;
        ESP_LOGI(TAG, "Raw card closed");
    }
    return s_card ? ESP_OK : sdcard_init();
}

void sdcard_deinit(void)
{
    if (!s_card) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    spi_bus_free(SD_HOST);
    s_card = NULL;
    ESP_LOGI(TAG, "Card unmounted");
}
