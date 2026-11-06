/*
 * Si5351A clock generator, written from the AN619 register description.
 *
 * Signal path: 25 MHz XTAL -> PLLA (fractional, f_vco = 600..900 MHz)
 * -> MS0 / MS1 (same even integer divider N) -> CLK0 / CLK1.
 *
 * Quadrature: the CLKx_PHOFF registers delay an output in steps of
 * 1 / (4 * f_vco). A quarter of the output period is N / (4 * f_vco), so
 * PHOFF = N gives exactly 90 degrees. PHOFF is 7 bits, hence N <= 126.
 * The offset only takes effect after a PLL reset.
 */
#include "si5351.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "si5351";

#define SI5351_I2C_HZ (400 * 1000)

/* Registers (AN619). */
#define REG_DEVICE_STATUS  0
#define REG_OUTPUT_ENABLE  3   /* bit n = 1 disables CLKn */
#define REG_CLK0_CTRL      16  /* CLK0..CLK7 control at 16..23 */
#define REG_PLLA_PARAMS    26  /* 8 bytes: MSNA P1/P2/P3 */
#define REG_MS0_PARAMS     42  /* 8 bytes each: MS0 at 42, MS1 at 50 */
#define REG_MS1_PARAMS     50
#define REG_SSC_PARAMS     149 /* bit 7 = spread spectrum enable */
#define REG_CLK0_PHOFF     165 /* CLK0..CLK5 phase offset at 165..170 */
#define REG_CLK1_PHOFF     166
#define REG_PLL_RESET      177
#define REG_XTAL_LOAD      183

#define STATUS_SYS_INIT    0x80 /* set while the chip is starting up */

/* CLKx control bits. */
#define CLK_PDN            0x80 /* power down */
#define CLK_MS_INT         0x40 /* multisynth integer mode */
#define CLK_MS_SRC_PLLB    0x20 /* 0 = PLLA */
#define CLK_SRC_MS         0x0C /* clock source = own multisynth */
#define CLK_IDRV_8MA       0x03

#define PLL_RESET_A        0x20
#define XTAL_LOAD_10PF     0xD2 /* bits 7:6 = 11, reserved bits per AN619 */

#define VCO_MIN_HZ  600000000ull
#define VCO_MAX_HZ  900000000ull
#define DIV_MIN     6
#define DIV_MAX     126 /* largest even N that fits the 7-bit PHOFF */
#define FRAC_DENOM  1048575u /* 20-bit c, maximum resolution */

static i2c_master_dev_handle_t s_dev;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), -1);
}

static esp_err_t write_regs(uint8_t reg, const uint8_t *vals, size_t len)
{
    uint8_t buf[1 + 8];
    buf[0] = reg;
    for (size_t i = 0; i < len; i++) {
        buf[1 + i] = vals[i];
    }
    return i2c_master_transmit(s_dev, buf, 1 + len, -1);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, -1);
}

/* Encode a + b/c into the 8-byte P1/P2/P3 block shared by PLL and
 * multisynth parameter registers (AN619 section 3.2 / 4.1.2). */
static void encode_params(uint32_t a, uint32_t b, uint32_t c, uint8_t out[8])
{
    uint32_t f = (uint32_t)(((uint64_t)128 * b) / c);
    uint32_t p1 = 128 * a + f - 512;
    uint32_t p2 = 128 * b - c * f;
    uint32_t p3 = c;

    out[0] = (p3 >> 8) & 0xFF;
    out[1] = p3 & 0xFF;
    out[2] = (p1 >> 16) & 0x03; /* R_DIV = 1, DIVBY4 = 0 */
    out[3] = (p1 >> 8) & 0xFF;
    out[4] = p1 & 0xFF;
    out[5] = (((p3 >> 16) & 0x0F) << 4) | ((p2 >> 16) & 0x0F);
    out[6] = (p2 >> 8) & 0xFF;
    out[7] = p2 & 0xFF;
}

esp_err_t si5351_init(i2c_master_bus_handle_t bus)
{
    esp_err_t ret = i2c_master_probe(bus, SI5351_ADDR, 50);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Si5351 not found at 0x%02x", SI5351_ADDR);
        return ret;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SI5351_ADDR,
        .scl_speed_hz = SI5351_I2C_HZ,
    };
    /* the handle stays from an earlier run of the radio */
    if (!s_dev && (ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev)) != ESP_OK) {
        return ret;
    }

    /* Wait for SYS_INIT to clear (chip finished power-up). */
    uint8_t status = STATUS_SYS_INIT;
    for (int i = 0; i < 100 && (status & STATUS_SYS_INIT); i++) {
        if ((ret = read_reg(REG_DEVICE_STATUS, &status)) != ESP_OK) goto fail;
        if (status & STATUS_SYS_INIT) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (status & STATUS_SYS_INIT) {
        ESP_LOGE(TAG, "Si5351 stuck in SYS_INIT");
        ret = ESP_ERR_TIMEOUT;
        goto fail;
    }

    /* All outputs off and powered down while configuring. */
    if ((ret = write_reg(REG_OUTPUT_ENABLE, 0xFF)) != ESP_OK) goto fail;
    for (int i = 0; i < 8; i++) {
        if ((ret = write_reg(REG_CLK0_CTRL + i, CLK_PDN)) != ESP_OK) goto fail;
    }
    if ((ret = write_reg(REG_SSC_PARAMS, 0x00)) != ESP_OK) goto fail;
    if ((ret = write_reg(REG_XTAL_LOAD, XTAL_LOAD_10PF)) != ESP_OK) goto fail;

    ESP_LOGI(TAG, "Si5351 found at 0x%02x (status 0x%02x)", SI5351_ADDR, status);
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return ret;
}

esp_err_t si5351_set_quadrature(uint32_t hz)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Largest even divider that keeps the VCO in range: more PHOFF headroom
     * is not needed, and a high VCO keeps PLL jitter low. */
    uint32_t n = (uint32_t)(VCO_MAX_HZ / hz) & ~1u;
    if (n > DIV_MAX) n = DIV_MAX;
    uint64_t vco = (uint64_t)n * hz;
    if (n < DIV_MIN || vco < VCO_MIN_HZ || vco > VCO_MAX_HZ) {
        ESP_LOGE(TAG, "%lu Hz out of quadrature range", (unsigned long)hz);
        return ESP_ERR_INVALID_ARG;
    }

    /* PLLA = XTAL * (a + b/c). */
    uint32_t a = (uint32_t)(vco / SI5351_XTAL_HZ);
    uint64_t rem = vco % SI5351_XTAL_HZ;
    uint32_t b = (uint32_t)((rem * FRAC_DENOM + SI5351_XTAL_HZ / 2) / SI5351_XTAL_HZ);
    uint32_t c = FRAC_DENOM;
    if (b >= c) { a++; b = 0; }

    uint8_t params[8];
    esp_err_t ret;
    encode_params(a, b, c, params);
    if ((ret = write_regs(REG_PLLA_PARAMS, params, 8)) != ESP_OK) return ret;

    /* MS0 = MS1 = N, integer mode (b = 0, c = 1). */
    encode_params(n, 0, 1, params);
    if ((ret = write_regs(REG_MS0_PARAMS, params, 8)) != ESP_OK) return ret;
    if ((ret = write_regs(REG_MS1_PARAMS, params, 8)) != ESP_OK) return ret;

    if ((ret = write_reg(REG_CLK0_PHOFF, 0)) != ESP_OK) return ret;
    if ((ret = write_reg(REG_CLK1_PHOFF, (uint8_t)n)) != ESP_OK) return ret;

    const uint8_t ctrl = CLK_MS_INT | CLK_SRC_MS | CLK_IDRV_8MA; /* from PLLA */
    if ((ret = write_reg(REG_CLK0_CTRL, ctrl)) != ESP_OK) return ret;
    if ((ret = write_reg(REG_CLK0_CTRL + 1, ctrl)) != ESP_OK) return ret;

    /* Reset PLLA so both multisynths restart together and PHOFF applies. */
    if ((ret = write_reg(REG_PLL_RESET, PLL_RESET_A)) != ESP_OK) return ret;
    if ((ret = write_reg(REG_OUTPUT_ENABLE, (uint8_t)~0x03)) != ESP_OK) return ret; /* CLK0, CLK1 on */

    ESP_LOGI(TAG, "CLK0/CLK1 = %lu Hz, 90 deg: VCO %llu Hz = %lu + %lu/%lu x XTAL, N = %lu",
             (unsigned long)hz, vco, (unsigned long)a, (unsigned long)b, (unsigned long)c,
             (unsigned long)n);
    return ESP_OK;
}
