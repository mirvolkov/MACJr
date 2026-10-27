/*
 * MCP23018 keyboard HAL: the expander sits on I2C (SDA=8, SCL=9) and is
 * polled, no interrupts.
 *
 * The keyboard is a non-standard matrix of 32 keys on connector pins 1..14:
 * rows are pins 1, 12, 13, 14 (all on port A), columns are pins 2..11 (port B
 * plus A2/A3). Rows 12/13/14 have a key on every column; row 1 only on
 * columns 5 and 6. The pin-to-expander mapping is in s_rows / s_cols.
 *
 * One row at a time is driven low by switching it to output (OLATA = 0,
 * outputs are open-drain); a pressed key pulls its column low against the
 * pull-up.
 *
 * Each debounced key press is published as a one-byte key code on the
 * character device /dev/kbrd. Every open() gets its own queue, so all
 * readers see every key.
 */
#include "keyboard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "keyboard";

#define KBD_SDA      8
#define KBD_SCL      9
#define KBD_I2C_HZ   (400 * 1000)

/* 7-bit address is 0x20 + ADDR pin voltage level (0..7); probed at init. */
#define MCP_ADDR_BASE 0x20

/* Register map with IOCON.BANK = 0 (power-on default): A at even, B at odd.
 * SEQOP = 0 by default, so a 2-byte read at GPIOA returns GPIOA, GPIOB. */
#define MCP_IODIRA   0x00
#define MCP_GPPUA    0x0C
#define MCP_GPIOA    0x12
#define MCP_OLATA    0x14

/* Expander pin as a bit in the 16-bit (GPIOB << 8 | GPIOA) word. */
#define PA(n) (n)
#define PB(n) (8 + (n))

#define KBD_ROWS        4
#define KBD_COLS        10
#define KBD_SCAN_MS     5      /* debounce = 2 matching scans */
/* At least one tick: with CONFIG_FREERTOS_HZ=100 a 5 ms delay rounds to 0. */
#define KBD_SCAN_TICKS  (pdMS_TO_TICKS(KBD_SCAN_MS) > 0 ? pdMS_TO_TICKS(KBD_SCAN_MS) : 1)

#define KBD_MAX_READERS 4      /* simultaneous open() of /dev/kbrd */
#define KBD_QUEUE_BYTES 32     /* per reader; keys are dropped when full */

typedef struct {
    uint8_t kbd_pin; /* keyboard connector pin */
    uint8_t mcp_bit; /* PA()/PB() */
} kbd_line_t;

/* Rows are driven, all on port A. */
static const kbd_line_t s_rows[KBD_ROWS] = {
    { 1, PA(1) }, { 12, PA(6) }, { 13, PA(0) }, { 14, PA(7) },
};

/* Columns are read, keyboard pins 2..11 in order. */
static const kbd_line_t s_cols[KBD_COLS] = {
    { 2, PA(2) }, { 3, PB(0) }, { 4, PB(6) }, { 5, PB(2) }, { 6, PA(3) },
    { 7, PB(3) }, { 8, PB(1) }, { 9, PB(4) }, { 10, PB(5) }, { 11, PB(7) },
};

/* Columns each row actually has keys on (bit c = s_cols[c]). Row pin 1 only
 * meets column pins 5 and 6; rows 12/13/14 meet all ten columns. */
#define COL_BIT(pin) (1u << ((pin) - 2))
static const uint16_t s_row_cols[KBD_ROWS] = {
    COL_BIT(5) | COL_BIT(6), 0x3FF, 0x3FF, 0x3FF,
};

typedef struct {
    StreamBufferHandle_t buf; /* NULL = slot free */
    bool nonblock;
} kbd_reader_t;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_keycode[KBD_ROWS][KBD_COLS]; /* 0 = no key at that spot */
static kbd_reader_t s_readers[KBD_MAX_READERS];
static SemaphoreHandle_t s_readers_lock;

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

static esp_err_t mcp_configure_matrix(void)
{
    esp_err_t ret;
    if ((ret = mcp_write_pair(MCP_IODIRA, 0xFF, 0xFF)) != ESP_OK) return ret;   /* all idle as inputs */
    if ((ret = mcp_write_pair(MCP_GPPUA, 0xFF, 0xFF)) != ESP_OK) return ret;    /* pull-ups */
    return mcp_write(MCP_OLATA, 0x00); /* a row switched to output drives low */
}

/* Number the existing keys row by row: KBD_KEYCODE_BASE .. +KBD_KEY_COUNT-1. */
static void keycodes_assign(void)
{
    uint8_t code = KBD_KEYCODE_BASE;
    for (int r = 0; r < KBD_ROWS; r++) {
        for (int c = 0; c < KBD_COLS; c++) {
            s_keycode[r][c] = (s_row_cols[r] & (1u << c)) ? code++ : 0;
        }
    }
}

/* Drive each row low in turn and read both ports. out[r] bit c = 1 means
 * the key at s_rows[r] x s_cols[c] is down. */
static esp_err_t matrix_scan(uint16_t out[KBD_ROWS])
{
    esp_err_t ret;
    for (int r = 0; r < KBD_ROWS; r++) {
        uint8_t row_bit = 1 << s_rows[r].mcp_bit;
        if ((ret = mcp_write(MCP_IODIRA, (uint8_t)~row_bit)) != ESP_OK) return ret;
        uint8_t ab[2];
        if ((ret = mcp_read(MCP_GPIOA, ab, 2)) != ESP_OK) return ret;
        uint16_t low = (uint16_t)~(ab[0] | (ab[1] << 8));
        uint16_t keys = 0;
        for (int c = 0; c < KBD_COLS; c++) {
            if (low & (1u << s_cols[c].mcp_bit)) keys |= 1u << c;
        }
        out[r] = keys & s_row_cols[r];
    }
    return mcp_write(MCP_IODIRA, 0xFF); /* release all rows */
}

/* Hand a key code to every open reader; a full queue drops the key. */
static void key_publish(uint8_t code)
{
    xSemaphoreTake(s_readers_lock, portMAX_DELAY);
    for (int i = 0; i < KBD_MAX_READERS; i++) {
        if (s_readers[i].buf) {
            xStreamBufferSend(s_readers[i].buf, &code, 1, 0);
        }
    }
    xSemaphoreGive(s_readers_lock);
}

static void scan_task(void *arg)
{
    uint16_t raw[KBD_ROWS], prev_raw[KBD_ROWS] = { 0 }, stable[KBD_ROWS] = { 0 };
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, KBD_SCAN_TICKS);
        if (matrix_scan(raw) != ESP_OK) {
            ESP_LOGW(TAG, "matrix scan I2C error");
            continue;
        }
        for (int r = 0; r < KBD_ROWS; r++) {
            /* Debounce: accept a row only after two identical scans. */
            uint16_t changed = (raw[r] == prev_raw[r]) ? (raw[r] ^ stable[r]) : 0;
            prev_raw[r] = raw[r];
            uint16_t pressed = changed & raw[r];
            for (int c = 0; c < KBD_COLS; c++) {
                if (pressed & (1u << c)) {
                    ESP_LOGD(TAG, "key %u (pin %d x pin %d)", s_keycode[r][c],
                             s_rows[r].kbd_pin, s_cols[c].kbd_pin);
                    key_publish(s_keycode[r][c]);
                }
            }
            stable[r] ^= changed;
        }
    }
}

/* ---- /dev/kbrd ---------------------------------------------------------- */

static bool reader_valid(int fd)
{
    return fd >= 0 && fd < KBD_MAX_READERS && s_readers[fd].buf;
}

static int kbrd_open(void *ctx, const char *path, int flags, int mode)
{
    if (path[0] != '\0' && strcmp(path, "/") != 0) {
        errno = ENOENT;
        return -1;
    }
    if ((flags & O_ACCMODE) != O_RDONLY) {
        errno = EACCES;
        return -1;
    }
    StreamBufferHandle_t buf = xStreamBufferCreate(KBD_QUEUE_BYTES, 1);
    if (!buf) {
        errno = ENOMEM;
        return -1;
    }
    xSemaphoreTake(s_readers_lock, portMAX_DELAY);
    for (int fd = 0; fd < KBD_MAX_READERS; fd++) {
        if (!s_readers[fd].buf) {
            s_readers[fd].buf = buf;
            s_readers[fd].nonblock = (flags & O_NONBLOCK) != 0;
            xSemaphoreGive(s_readers_lock);
            return fd;
        }
    }
    xSemaphoreGive(s_readers_lock);
    vStreamBufferDelete(buf);
    errno = ENFILE;
    return -1;
}

static ssize_t kbrd_read(void *ctx, int fd, void *dst, size_t size)
{
    if (!reader_valid(fd)) {
        errno = EBADF;
        return -1;
    }
    if (size == 0) {
        return 0;
    }
    kbd_reader_t *rd = &s_readers[fd];
    size_t n = xStreamBufferReceive(rd->buf, dst, size, rd->nonblock ? 0 : portMAX_DELAY);
    if (n == 0) {
        errno = EAGAIN;
        return -1;
    }
    return (ssize_t)n;
}

static int kbrd_close(void *ctx, int fd)
{
    if (!reader_valid(fd)) {
        errno = EBADF;
        return -1;
    }
    xSemaphoreTake(s_readers_lock, portMAX_DELAY);
    StreamBufferHandle_t buf = s_readers[fd].buf;
    s_readers[fd].buf = NULL;
    xSemaphoreGive(s_readers_lock);
    vStreamBufferDelete(buf);
    return 0;
}

static int kbrd_fcntl(void *ctx, int fd, int cmd, int arg)
{
    if (!reader_valid(fd)) {
        errno = EBADF;
        return -1;
    }
    switch (cmd) {
    case F_GETFL:
        return O_RDONLY | (s_readers[fd].nonblock ? O_NONBLOCK : 0);
    case F_SETFL:
        s_readers[fd].nonblock = (arg & O_NONBLOCK) != 0;
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

static int kbrd_fstat(void *ctx, int fd, struct stat *st)
{
    if (!reader_valid(fd)) {
        errno = EBADF;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0444;
    return 0;
}

static const esp_vfs_fs_ops_t s_kbrd_ops = {
    .open_p = kbrd_open,
    .read_p = kbrd_read,
    .close_p = kbrd_close,
    .fcntl_p = kbrd_fcntl,
    .fstat_p = kbrd_fstat,
};

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

    if ((ret = mcp_find()) != ESP_OK) goto fail_bus;
    if ((ret = mcp_configure_matrix()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure MCP23018 matrix: %s", esp_err_to_name(ret));
        goto fail_bus;
    }
    keycodes_assign();

    s_readers_lock = xSemaphoreCreateMutex();
    if (!s_readers_lock) {
        ret = ESP_ERR_NO_MEM;
        goto fail_bus;
    }
    ret = esp_vfs_register_fs(KBD_DEV_PATH, &s_kbrd_ops,
                              ESP_VFS_FLAG_CONTEXT_PTR | ESP_VFS_FLAG_STATIC, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register %s: %s", KBD_DEV_PATH, esp_err_to_name(ret));
        goto fail_lock;
    }
    if (xTaskCreate(scan_task, "kbd_scan", 3072, NULL, 5, NULL) != pdPASS) {
        esp_vfs_unregister(KBD_DEV_PATH);
        ret = ESP_ERR_NO_MEM;
        goto fail_lock;
    }

    ESP_LOGI(TAG, "Keyboard ready on %s: %d keys, codes %d..%d, scan %lu ms",
             KBD_DEV_PATH, KBD_KEY_COUNT, KBD_KEYCODE_BASE, KBD_KEYCODE_BASE + KBD_KEY_COUNT - 1,
             (unsigned long)(KBD_SCAN_TICKS * portTICK_PERIOD_MS));
    return ESP_OK;

fail_lock:
    vSemaphoreDelete(s_readers_lock);
    s_readers_lock = NULL;
fail_bus:
    if (s_dev) i2c_master_bus_rm_device(s_dev);
    i2c_del_master_bus(s_bus);
    s_dev = NULL;
    s_bus = NULL;
    return ret;
}
