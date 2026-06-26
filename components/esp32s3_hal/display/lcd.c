#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lcd.h"

/*
 * Low-level ST7789 SPI panel driver: brings up SPI3 + the panel, and exposes
 * a synchronous blit (lcd_blit_sync) that blocks until DMA finishes so
 * display_hal.c can safely reuse its row buffer right after the call returns.
 */

// Pin definitions (ESP32-S3, ST7789)
#define ST7789_CS  1   // Chip select
#define ST7789_DC  45  // Data/Command
#define ST7789_SCK 47  // SPI clock
#define ST7789_SDA 21  // SPI data (MOSI)
#define ST7789_RST 35  // Reset

// Display / SPI configuration
#define LCD_HOST           SPI3_HOST
#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000) // 80 MHz
#define LCD_CMD_BITS       8
#define LCD_PARAM_BITS     8

// Largest single transfer the bus must size for (also the fill-screen chunk).
#define BUFFER_LINES 40

static const char *TAG = "ST7789";

esp_lcd_panel_io_handle_t io_handle = NULL;
esp_lcd_panel_handle_t panel_handle = NULL;

// Signaled from the SPI "color transfer done" ISR; lets lcd_blit_sync() block
// until the panel has finished reading the caller's buffer.
static SemaphoreHandle_t s_trans_done = NULL;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    if (s_trans_done) {
        xSemaphoreGiveFromISR(s_trans_done, &woken);
    }
    return woken == pdTRUE;
}

// RGB888 -> RGB565
uint16_t rgb_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void lcd_blit_sync(int x1, int y1, int x2, int y2, const void *buf)
{
    // Drop any stale completion left over from a prior transfer, then draw and
    // wait for THIS transfer's completion before returning.
    xSemaphoreTake(s_trans_done, 0);
    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2, y2, buf);
    xSemaphoreTake(s_trans_done, portMAX_DELAY);
}

esp_err_t lcd_init(void)
{
    ESP_LOGI(TAG, "Initializing SPI3 bus at %d MHz", LCD_PIXEL_CLOCK_HZ / 1000000);
    ESP_LOGI(TAG, "Pins: CS=%d DC=%d SCK=%d SDA=%d RST=%d",
             ST7789_CS, ST7789_DC, ST7789_SCK, ST7789_SDA, ST7789_RST);

    s_trans_done = xSemaphoreCreateBinary();
    if (!s_trans_done) {
        ESP_LOGE(TAG, "failed to create trans-done semaphore");
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t buscfg = {
        .mosi_io_num = ST7789_SDA,
        .miso_io_num = -1,
        .sclk_io_num = ST7789_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * BUFFER_LINES * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Installing panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = ST7789_CS,
        .dc_gpio_num = ST7789_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .flags = {
            .dc_low_on_data = 0,
            .octal_mode = 0,
            .lsb_first = 0,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Installing ST7789 panel driver");
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = ST7789_RST,
        /* This module's panel is wired BGR, not RGB -- with RGB here, red
           and blue come out swapped (confirmed against the macOS/SDL host
           build, which renders the same RGB888 values straight). */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    /* This ST7789 panel needs INVON to show true colors -- without it, black
       cells render white (and vice versa). */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    /* Landscape: swap the panel's native x/y axes (it's a portrait-native
       controller) to match LCD_H_RES/LCD_V_RES above. */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    /* Mirror the (post-swap) vertical axis to correct the panel's physical
       orientation in its enclosure. */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "LCD ready: %dx%d @ %d MHz", LCD_H_RES, LCD_V_RES, LCD_PIXEL_CLOCK_HZ / 1000000);
    return ESP_OK;
}
