/*
 * Shared I2C master bus. Created lazily by the first driver that needs it
 * and never torn down, so drivers only add/remove their own devices.
 */
#include "i2c_bus.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "i2c_bus";

#define I2C_BUS_SDA 8
#define I2C_BUS_SCL 9

static i2c_master_bus_handle_t s_bus;
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_lock_mux = portMUX_INITIALIZER_UNLOCKED;

esp_err_t i2c_bus_get(i2c_master_bus_handle_t *out)
{
    /* First call happens from app_main before any driver task exists, but
     * guard creation anyway in case two drivers race. */
    taskENTER_CRITICAL(&s_lock_mux);
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    taskEXIT_CRITICAL(&s_lock_mux);

    esp_err_t ret = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_bus) {
        i2c_master_bus_config_t cfg = {
            .i2c_port = -1, /* auto-select */
            .sda_io_num = I2C_BUS_SDA,
            .scl_io_num = I2C_BUS_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ret = i2c_new_master_bus(&cfg, &s_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
            s_bus = NULL;
        } else {
            ESP_LOGI(TAG, "I2C bus up (SDA=%d, SCL=%d)", I2C_BUS_SDA, I2C_BUS_SCL);
        }
    }
    xSemaphoreGive(s_lock);
    *out = s_bus;
    return ret;
}
