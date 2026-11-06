/*
 * MCP23018 keyboard HAL: the expander sits on the shared I2C bus (i2c_bus) and is
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
 * character device /dev/kbrd; Cx as KEY_ESC. A key that starts a chord
 * (s_chords) is held back while it is down: its partner turns the pair
 * into the chord's code, a release or any other key lets it through.
 * Two keys are always read correctly; the matrix has no diodes, so three
 * keys on the corners of a rectangle would show a fourth (ghosting).
 * Every open() gets its own queue, so all
 * readers see every key. select() reports a descriptor readable while its
 * queue has keys: each select() in progress is registered here, and
 * key_publish() wakes the ones watching a queue that just got a key.
 */
#include "keyboard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>

#include "driver/i2c_master.h"
#include "i2c_bus.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "keyboard";

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

/* Simultaneous open() of /dev/kbrd. While a program runs the firmware
 * holds 4 (key beep, monitor, exec's kill watch and sys_getkey() queue). */
#define KBD_MAX_READERS 8
#define KBD_QUEUE_BYTES 32     /* per reader; keys are dropped when full */
#define KBD_MAX_SELECTS 4      /* select() calls waiting at the same time */
#define KBD_RAW_CX      140    /* matrix code of Cx, published as KEY_ESC */

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

/* Two keys pressed together, published as `code`. */
typedef struct {
    uint8_t a, b, code;
} kbd_chord_t;

static const kbd_chord_t s_chords[] = {
    { KEY_MK_F, KEY_MK_K, KEY_MK_F_K },
    { KEY_MK_F, KEY_ESC, KEY_MK_F_ESC },
};

typedef struct {
    StreamBufferHandle_t buf; /* NULL = slot free */
    bool nonblock;
} kbd_reader_t;

/* A select() waiting on some of our descriptors. */
typedef struct {
    esp_vfs_select_sem_t sem;
    fd_set *readfds; /* the caller's set: ready descriptors go here */
    fd_set watched;  /* the descriptors it asked about */
} kbd_select_t;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_keycode[KBD_ROWS][KBD_COLS]; /* 0 = no key at that spot */
static kbd_reader_t s_readers[KBD_MAX_READERS];
static volatile uint16_t s_stable[KBD_ROWS]; /* debounced state, bit c = key down */
static volatile uint32_t s_scans;            /* completed scans since init */
static kbd_select_t *s_selects[KBD_MAX_SELECTS]; /* under s_readers_lock */
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

/* Number the existing keys row by row: KBD_KEYCODE_BASE .. +KBD_KEY_COUNT-1,
 * except Cx, which is KEY_ESC. */
static void keycodes_assign(void)
{
    uint8_t code = KBD_KEYCODE_BASE;
    for (int r = 0; r < KBD_ROWS; r++) {
        for (int c = 0; c < KBD_COLS; c++) {
            if (s_row_cols[r] & (1u << c)) {
                s_keycode[r][c] = code == KBD_RAW_CX ? KEY_ESC : code;
                code++;
            } else {
                s_keycode[r][c] = 0;
            }
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

/* Wake the select() calls watching reader `fd`. Under s_readers_lock. */
static void select_notify(int fd)
{
    for (int i = 0; i < KBD_MAX_SELECTS; i++) {
        kbd_select_t *sel = s_selects[i];
        if (sel && FD_ISSET(fd, &sel->watched)) {
            FD_SET(fd, sel->readfds);
            esp_vfs_select_triggered(sel->sem);
        }
    }
}

/* Hand a key code to every open reader; a full queue drops the key. */
static void key_publish(uint8_t code)
{
    xSemaphoreTake(s_readers_lock, portMAX_DELAY);
    for (int i = 0; i < KBD_MAX_READERS; i++) {
        if (s_readers[i].buf && xStreamBufferSend(s_readers[i].buf, &code, 1, 0) == 1) {
            select_notify(i);
        }
    }
    xSemaphoreGive(s_readers_lock);
}

/* The chord `a` and `b` make, or 0. */
static uint8_t chord_of(uint8_t a, uint8_t b)
{
    for (size_t i = 0; i < sizeof(s_chords) / sizeof(s_chords[0]); i++) {
        const kbd_chord_t *ch = &s_chords[i];
        if ((ch->a == a && ch->b == b) || (ch->a == b && ch->b == a)) {
            return ch->code;
        }
    }
    return 0;
}

static bool starts_chord(uint8_t code)
{
    for (size_t i = 0; i < sizeof(s_chords) / sizeof(s_chords[0]); i++) {
        if (s_chords[i].a == code || s_chords[i].b == code) {
            return true;
        }
    }
    return false;
}

/* A key went down: publish it, hold it back as a possible chord, or
 * complete the chord the held key started. `*held`: the key held back, 0
 * if none. */
static void key_down(uint8_t code, uint8_t *held)
{
    if (*held) {
        uint8_t chord = chord_of(*held, code);
        if (chord) {
            *held = 0;
            key_publish(chord);
            return;
        }
        key_publish(*held); /* not a chord: the held key came first */
        *held = 0;
    }
    if (starts_chord(code)) {
        *held = code;
    } else {
        key_publish(code);
    }
}

/* A key went up: if it was held back, it was pressed alone. */
static void key_up(uint8_t code, uint8_t *held)
{
    if (*held == code) {
        *held = 0;
        key_publish(code);
    }
}

static void scan_task(void *arg)
{
    uint8_t held = 0; /* key held back as the start of a chord */
    uint16_t raw[KBD_ROWS], prev_raw[KBD_ROWS] = { 0 };
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, KBD_SCAN_TICKS);
        if (matrix_scan(raw) != ESP_OK) {
            ESP_LOGW(TAG, "matrix scan I2C error");
            continue;
        }
        for (int r = 0; r < KBD_ROWS; r++) {
            /* Debounce: accept a row only after two identical scans. */
            uint16_t changed = (raw[r] == prev_raw[r]) ? (raw[r] ^ s_stable[r]) : 0;
            prev_raw[r] = raw[r];
            for (int c = 0; c < KBD_COLS; c++) {
                if (!(changed & (1u << c))) {
                    continue;
                }
                if (raw[r] & (1u << c)) {
                    ESP_LOGD(TAG, "key %u (pin %d x pin %d)", s_keycode[r][c],
                             s_rows[r].kbd_pin, s_cols[c].kbd_pin);
                    key_down(s_keycode[r][c], &held);
                } else {
                    key_up(s_keycode[r][c], &held);
                }
            }
            s_stable[r] ^= changed;
        }
        s_scans++;
    }
}

bool keyboard_key_down(uint8_t code)
{
    if (!s_dev) {
        return false;
    }
    /* A key held since power-up needs two matching scans to be accepted. */
    for (int i = 0; s_scans < 2 && i < 10; i++) {
        vTaskDelay(KBD_SCAN_TICKS);
    }
    for (int r = 0; r < KBD_ROWS; r++) {
        for (int c = 0; c < KBD_COLS; c++) {
            if (s_keycode[r][c] == code) {
                return (s_stable[r] & (1u << c)) != 0;
            }
        }
    }
    return false;
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
    ESP_LOGW(TAG, "%s: all %d readers taken", KBD_DEV_PATH, KBD_MAX_READERS);
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

/* select() on /dev/kbrd: readable while the reader's queue has keys.
 * Never writable, no exceptions. The VFS hands us its sets with our own
 * descriptor numbers; they are cleared here and refilled as descriptors
 * become ready. */
static esp_err_t kbrd_start_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
                                   esp_vfs_select_sem_t sem, void **end_select_args)
{
    *end_select_args = NULL;
    int max_fd = nfds < KBD_MAX_READERS ? nfds : KBD_MAX_READERS;
    kbd_select_t *sel = calloc(1, sizeof(*sel));
    if (!sel) {
        return ESP_ERR_NO_MEM;
    }
    sel->sem = sem;
    sel->readfds = readfds;
    FD_ZERO(&sel->watched);

    xSemaphoreTake(s_readers_lock, portMAX_DELAY);
    for (int fd = 0; fd < max_fd; fd++) {
        if (FD_ISSET(fd, readfds)) {
            if (!reader_valid(fd)) {
                xSemaphoreGive(s_readers_lock);
                free(sel);
                return ESP_ERR_INVALID_STATE; /* select() fails with EBADF */
            }
            FD_SET(fd, &sel->watched);
        }
    }
    int slot = 0;
    while (slot < KBD_MAX_SELECTS && s_selects[slot]) {
        slot++;
    }
    if (slot == KBD_MAX_SELECTS) {
        xSemaphoreGive(s_readers_lock);
        free(sel);
        return ESP_ERR_NO_MEM;
    }
    FD_ZERO(readfds);
    FD_ZERO(writefds);
    FD_ZERO(exceptfds);
    s_selects[slot] = sel;
    for (int fd = 0; fd < max_fd; fd++) {
        if (FD_ISSET(fd, &sel->watched) && xStreamBufferBytesAvailable(s_readers[fd].buf) > 0) {
            select_notify(fd); /* keys already waiting */
        }
    }
    xSemaphoreGive(s_readers_lock);
    *end_select_args = sel;
    return ESP_OK;
}

static esp_err_t kbrd_end_select(void *end_select_args)
{
    kbd_select_t *sel = end_select_args;
    if (!sel) {
        return ESP_OK;
    }
    xSemaphoreTake(s_readers_lock, portMAX_DELAY);
    for (int i = 0; i < KBD_MAX_SELECTS; i++) {
        if (s_selects[i] == sel) {
            s_selects[i] = NULL;
        }
    }
    xSemaphoreGive(s_readers_lock);
    free(sel);
    return ESP_OK;
}

static const esp_vfs_select_ops_t s_kbrd_select_ops = {
    .start_select = kbrd_start_select,
    .end_select = kbrd_end_select,
};

static const esp_vfs_fs_ops_t s_kbrd_ops = {
    .open_p = kbrd_open,
    .read_p = kbrd_read,
    .close_p = kbrd_close,
    .fcntl_p = kbrd_fcntl,
    .fstat_p = kbrd_fstat,
    .select = &s_kbrd_select_ops,
};

esp_err_t keyboard_init(void)
{
    ESP_LOGI(TAG, "Initializing MCP23018 keyboard");

    esp_err_t ret = i2c_bus_get(&s_bus);
    if (ret != ESP_OK) {
        return ret;
    }

    if ((ret = mcp_find()) != ESP_OK) goto fail_dev;
    if ((ret = mcp_configure_matrix()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure MCP23018 matrix: %s", esp_err_to_name(ret));
        goto fail_dev;
    }
    keycodes_assign();

    s_readers_lock = xSemaphoreCreateMutex();
    if (!s_readers_lock) {
        ret = ESP_ERR_NO_MEM;
        goto fail_dev;
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
fail_dev:
    if (s_dev) i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return ret;
}
