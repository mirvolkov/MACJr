/*
 * ESP32-S3 Wi-Fi-band SDR capture -- see espsdr.h.
 *
 * The receiver's baseband ADC feeds a factory debug "dump engine" that copies
 * one 32-bit IQ word per sample into a fixed SRAM aperture. We reserve that
 * aperture at boot, put the PHY into a plain promiscuous-RX state with forced
 * gain (no recalibration -- fine for a relative-power spectrogram), trigger a
 * one-shot capture, and read the samples back.
 *
 * Registers and sequence after ESPARGOS esp-sdr (docs/how-it-works.md,
 * targets/esp32s3/receiver.c). Snapshot path only; no continuous ring.
 */
#include "espsdr.h"

#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "net.h"
#include "soc/soc.h"
#include "heap_memory_layout.h"

/* S3 adctrig snapshot aperture (MAC_DUMP_USAGE=4): 64 KB. Reserved so the
 * heap and static sections never land in the bank the dump engine writes. */
#define IQ_BASE   0x3fcd0000u
#define IQ_END    0x3fce0000u
#define IQ_BUFFER ((volatile uint32_t *)IQ_BASE)
SOC_RESERVE_MEMORY_REGION(IQ_BASE, IQ_END, espsdr_dump);

#define SRAM_OWNER_REG 0x600c101cu
#define DUMP_CTRL_REG  0x60033d5cu
#define DUMP_LANE_REG  0x60033d90u
#define DUMP_DONE_BIT  (1u << 18)
#define DUMP_TRIG_BIT  (1u << 19)
#define SENTINEL       0xa5a0055au /* marks words the engine never wrote */

static const char *TAG = "espsdr";

/* PHY register helpers from librftest.a (CONFIG_ESP_PHY_ENABLE_CERT_TEST). */
extern void stop_tx_tone(unsigned);
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(unsigned);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(unsigned);
extern void set_chanfreq(unsigned, unsigned);
extern void force_rx_gain(unsigned, unsigned, unsigned);

static bool s_on;

esp_err_t espsdr_start(void) {
    if (s_on) {
        return ESP_OK;
    }
    esp_err_t err = net_wifi_up(); /* ensure the PHY/Wi-Fi is running */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi not up: %s", esp_err_to_name(err));
        return err;
    }
    /* promiscuous RX on a fixed channel, TX off, RX clock on, forced gain */
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    stop_tx_tone(1);
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    esp_rom_delay_us(3000);
    force_rx_gain(1, 55, 0);
    s_on = true;
    ESP_LOGI(TAG, "capture on");
    return ESP_OK;
}

void espsdr_tune(unsigned mhz) {
    set_chanfreq(mhz, 0);
    force_rx_gain(1, 55, 0);
}

int espsdr_snapshot(uint32_t *out, unsigned n, espsdr_rate_t rate) {
    if (!s_on || n == 0 || n > ESPSDR_MAX_WORDS) {
        return -1;
    }
    for (unsigned j = 0; j < n; j++) {
        IQ_BUFFER[j] = SENTINEL;
    }
    uint32_t owner = REG_READ(SRAM_OWNER_REG);
    REG_WRITE(DUMP_CTRL_REG, 0);
    REG_WRITE(DUMP_LANE_REG, 0u | (1u << 6) | (2u << 12) | (3u << 18)); /* record I,Q */
    REG_WRITE(SRAM_OWNER_REG, (owner & ~15u) | 4u);                     /* engine owns the bank */

    /* bit15 -> 40 MS/s, bit16 -> 16 MS/s, neither -> 80 MS/s. */
    uint32_t rate_bits = rate == ESPSDR_RATE_40M ? (1u << 15) : rate == ESPSDR_RATE_16M ? (1u << 16) : 0u;
    uint32_t ctrl = 0x80000000u | rate_bits | n; /* enable + sample count */
    REG_WRITE(DUMP_CTRL_REG, ctrl);
    REG_WRITE(DUMP_CTRL_REG, ctrl | DUMP_TRIG_BIT); /* trigger as a pulse */
    REG_WRITE(DUMP_CTRL_REG, ctrl);

    int64_t start = esp_timer_get_time();
    while (!(REG_READ(DUMP_CTRL_REG) & DUMP_DONE_BIT) && esp_timer_get_time() - start < 20000) {
    }
    bool done = (REG_READ(DUMP_CTRL_REG) & DUMP_DONE_BIT) != 0;
    REG_WRITE(DUMP_CTRL_REG, 0);
    REG_WRITE(SRAM_OWNER_REG, owner);
    if (!done) {
        return -1;
    }
    for (unsigned j = 0; j < n; j++) {
        if (IQ_BUFFER[j] == SENTINEL) {
            return -1; /* capture came up short */
        }
    }
    memcpy(out, (const void *)IQ_BUFFER, (size_t)n * 4);
    return (int)n;
}

void espsdr_stop(void) {
    if (!s_on) {
        return;
    }
    esp_wifi_set_promiscuous(false);
    s_on = false;
    ESP_LOGI(TAG, "capture off");
}
