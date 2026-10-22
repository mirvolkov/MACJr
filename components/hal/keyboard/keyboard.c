/*
 * MCP23018 keyboard HAL: the expander sits on I2C (SDA=8, SCL=9) with its
 * INTA -> GPIO3 and INTB -> GPIO46. Both ports are configured as inputs with
 * pull-ups and interrupt-on-change; INT pins are open-drain, active low.
 *
 * The GPIO ISR only posts the pin number to a queue; a worker task does the
 * I2C reads (INTF/INTCAP, which also clears the interrupt) and the logging.
 *
 * Matrix test mode instead polls a 4x8 key matrix: rows are GPA7..GPA4
 * (row 0 = GPA7), columns are GPB0..GPB7 (col 0 = GPB0). One row at a time
 * is driven low by switching it to output (OLATA = 0, outputs are
 * open-drain); a pressed key pulls its column low against the pull-up.
 */
#include "keyboard.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "keyboard";

#define KBD_SDA      8
#define KBD_SCL      9
#define KBD_INTA     3
#define KBD_INTB     46
#define KBD_I2C_HZ   (400 * 1000)

/* 7-bit address is 0x20 + ADDR pin voltage level (0..7); probed at init. */
#define MCP_ADDR_BASE 0x20

/* Register map with IOCON.BANK = 0 (power-on default): A at even, B at odd. */
#define MCP_IODIRA   0x00
#define MCP_IODIRB   0x01
#define MCP_GPINTENA 0x04
#define MCP_INTCONA  0x08
#define MCP_IOCON    0x0A
#define MCP_GPPUA    0x0C
#define MCP_INTFA    0x0E
#define MCP_INTCAPA  0x10
#define MCP_GPIOA    0x12
#define MCP_GPIOB    0x13
#define MCP_OLATA    0x14

#define KBD_ROWS        4
#define KBD_COLS        8
#define KBD_ROW_SHIFT   4      /* rows on GPA4..GPA7 */
#define KBD_ROW_MASK    0xF0
#define KBD_SCAN_MS     5      /* debounce = 2 matching scans */
/* At least one tick: with CONFIG_FREERTOS_HZ=100 a 5 ms delay rounds to 0. */
#define KBD_SCAN_TICKS  (pdMS_TO_TICKS(KBD_SCAN_MS) > 0 ? pdMS_TO_TICKS(KBD_SCAN_MS) : 1)

/* IOCON: SEQOP=0 (auto-increment), ODR=1 (open-drain INT), INTCC=1 (reading
 * INTCAP clears the interrupt). MIRROR=0 so A and B stay on separate lines. */
#define MCP_IOCON_ODR   (1 << 2)
#define MCP_IOCON_INTCC (1 << 0)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static QueueHandle_t s_irq_queue;

static esp_err_t mcp_write_pair(uint8_t reg, uint8_t a, uint8_t b)
{
    uint8_t buf[3] = { reg, a, b };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), -1);
}

static esp_err_t mcp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), -1);
}

static esp_err_t mcp_read(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, len, -1);
}

static void IRAM_ATTR kbd_isr(void *arg)
{
    uint32_t pin = (uint32_t)arg;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_irq_queue, &pin, &woken);
    portYIELD_FROM_ISR(woken);
}

static void kbd_task(void *arg)
{
    uint32_t pin;
    for (;;) {
        if (xQueueReceive(s_irq_queue, &pin, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* INTFA, INTFB, then (auto-increment) skip to INTCAPA, INTCAPB. */
        uint8_t intf[2] = { 0 }, intcap[2] = { 0 };
        if (mcp_read(MCP_INTFA, intf, 2) != ESP_OK ||
            mcp_read(MCP_INTCAPA, intcap, 2) != ESP_OK) {
            ESP_LOGW(TAG, "MCP23018 read failed after IRQ on GPIO%lu", pin);
            continue;
        }
        ESP_LOGI(TAG, "key pressed (INT%c/GPIO%lu) INTF=%02x%02x CAP=%02x%02x",
                 pin == KBD_INTA ? 'A' : 'B', pin,
                 intf[1], intf[0], intcap[1], intcap[0]);
    }
}

static esp_err_t mcp_find(void)
{
    for (uint8_t addr = MCP_ADDR_BASE; addr < MCP_ADDR_BASE + 8; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "MCP23018 found at 0x%02x", addr);
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = addr,
                .scl_speed_hz = KBD_I2C_HZ,
            };
            return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
        }
    }
    ESP_LOGE(TAG, "MCP23018 not found on 0x20..0x27");
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t mcp_configure_irq(void)
{
    /* IOCON is mirrored at 0x0A/0x0B; write both copies. */
    uint8_t iocon = MCP_IOCON_ODR | MCP_IOCON_INTCC;
    esp_err_t ret;
    if ((ret = mcp_write_pair(MCP_IOCON, iocon, iocon)) != ESP_OK) return ret;
    if ((ret = mcp_write_pair(MCP_IODIRA, 0xFF, 0xFF)) != ESP_OK) return ret;   /* all inputs */
    if ((ret = mcp_write_pair(MCP_GPPUA, 0xFF, 0xFF)) != ESP_OK) return ret;    /* pull-ups */
    if ((ret = mcp_write_pair(MCP_INTCONA, 0x00, 0x00)) != ESP_OK) return ret;  /* on change */
    if ((ret = mcp_write_pair(MCP_GPINTENA, 0xFF, 0xFF)) != ESP_OK) return ret; /* enable IRQ */

    /* Clear any interrupt latched during configuration. */
    uint8_t dummy[2];
    mcp_read(MCP_INTCAPA, dummy, 2);
    mcp_read(MCP_GPIOA, dummy, 2);
    return ESP_OK;
}

static esp_err_t mcp_configure_matrix(void)
{
    esp_err_t ret;
    if ((ret = mcp_write_pair(MCP_GPINTENA, 0x00, 0x00)) != ESP_OK) return ret; /* no IRQs */
    if ((ret = mcp_write_pair(MCP_IODIRA, 0xFF, 0xFF)) != ESP_OK) return ret;   /* all idle as inputs */
    if ((ret = mcp_write_pair(MCP_GPPUA, 0xFF, 0xFF)) != ESP_OK) return ret;    /* pull-ups */
    return mcp_write(MCP_OLATA, 0x00); /* a row switched to output drives low */
}

/* Drive each row low in turn and read the columns. out[r] bit c = 1 means
 * key (r, c) is down. */
static esp_err_t matrix_scan(uint8_t out[KBD_ROWS])
{
    esp_err_t ret;
    for (int r = 0; r < KBD_ROWS; r++) {
        uint8_t row_bit = 1 << (KBD_ROW_SHIFT + KBD_ROWS - 1 - r); /* row 0 = GPA7 */
        if ((ret = mcp_write(MCP_IODIRA, (uint8_t)~row_bit)) != ESP_OK) return ret;
        uint8_t cols;
        if ((ret = mcp_read(MCP_GPIOB, &cols, 1)) != ESP_OK) return ret;
        out[r] = (uint8_t)~cols;
    }
    return mcp_write(MCP_IODIRA, 0xFF); /* release all rows */
}

static void matrix_task(void *arg)
{
    uint8_t raw[KBD_ROWS], prev_raw[KBD_ROWS] = { 0 }, stable[KBD_ROWS] = { 0 };
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, KBD_SCAN_TICKS);
        if (matrix_scan(raw) != ESP_OK) {
            ESP_LOGW(TAG, "matrix scan I2C error");
            continue;
        }
        for (int r = 0; r < KBD_ROWS; r++) {
            /* Debounce: accept a row only after two identical scans. */
            uint8_t changed = (raw[r] == prev_raw[r]) ? (raw[r] ^ stable[r]) : 0;
            prev_raw[r] = raw[r];
            for (int c = 0; c < KBD_COLS; c++) {
                if (changed & (1 << c)) {
                    ESP_LOGI(TAG, "key %s: row %d x col %d",
                             (raw[r] & (1 << c)) ? "down" : "up", r, c);
                }
            }
            stable[r] ^= changed;
        }
    }
}

esp_err_t keyboard_init(void)
{
    ESP_LOGI(TAG, "Initializing MCP23018 keyboard");

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1, /* auto-select */
        .sda_io_num = KBD_SDA,
        .scl_io_num = KBD_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    if ((ret = mcp_find()) != ESP_OK) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    return ret;
}

esp_err_t keyboard_irq_start(void)
{
    esp_err_t ret = mcp_configure_irq();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure MCP23018: %s", esp_err_to_name(ret));
        return ret;
    }

    s_irq_queue = xQueueCreate(16, sizeof(uint32_t));
    if (!s_irq_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(kbd_task, "kbd", 3072, NULL, 10, NULL) != pdPASS) return ESP_ERR_NO_MEM;

    /* INT lines are open-drain active-low: pull up and trigger on falling edge. */
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << KBD_INTA) | (1ULL << KBD_INTB),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    if ((ret = gpio_config(&io_cfg)) != ESP_OK) return ret;

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret; /* already installed is fine */
    gpio_isr_handler_add(KBD_INTA, kbd_isr, (void *)KBD_INTA);
    gpio_isr_handler_add(KBD_INTB, kbd_isr, (void *)KBD_INTB);

    ESP_LOGI(TAG, "Keyboard IRQ mode ready (INTA=GPIO%d, INTB=GPIO%d)", KBD_INTA, KBD_INTB);
    return ESP_OK;
}

esp_err_t keyboard_matrix_test_start(void)
{
    esp_err_t ret = mcp_configure_matrix();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure MCP23018 matrix: %s", esp_err_to_name(ret));
        return ret;
    }
    if (xTaskCreate(matrix_task, "kbd_scan", 3072, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Matrix test mode: %dx%d, rows GPA7..GPA4, cols GPB0..GPB7, scan %lu ms",
             KBD_ROWS, KBD_COLS, (unsigned long)(KBD_SCAN_TICKS * portTICK_PERIOD_MS));
    return ESP_OK;
}
