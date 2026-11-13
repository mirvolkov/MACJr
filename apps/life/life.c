/*
 * LIFE.ELF -- Conway's Game of Life on the full 320x240 panel.
 *
 * The engine is bit-parallel SWAR (life_swar.c): one bit per cell, 32 cells
 * advanced per word. Each generation is packed into the HAL's 1-bpp mask and
 * blitted in a single call -- the panel maps bit=1 to the live colour, so no
 * RGB buffer is needed. Draws on the display HAL directly (like the PNG viewer
 * and the SDR spectrogram), not through the tty.
 *
 *   argv[1]  seed sparseness s: a 2^-s live fraction (1..6, default 3 -> 12.5%)
 *
 * Controls (MK-52 keys):
 *   Cx    exit            В↑   reseed (random)
 *   С/П   run / pause      ШГ→  single step (while paused)
 *   +/-   faster / slower  x/÷  denser / sparser soup (reseeds)
 */
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "macjr.h"
#include "keyboard.h"
#include "monitor.h"
#include "life_swar.h"

#define GW        LIFE_GW
#define GH        LIFE_GH
#define WW        LIFE_WW
#define ROW_BYTES (GW / 8)       /* 40 */

#define FG_RGB 0x33ff66u         /* live cell: green */
#define BG_RGB 0x000000u         /* dead cell: black */

#define DELAY_MIN   5
#define DELAY_MAX 200
#define DELAY_DEF  33            /* ~30 generations/s */

/* Seed density = 2^-sparse: each extra draw AND'd in halves the live fraction.
 * 1 -> 50%, 2 -> 25%, 3 -> 12.5% (default), ... */
#define SPARSE_MIN   1
#define SPARSE_MAX   6
#define SPARSE_DEF   3

static uint8_t *s_mask;          /* GH * ROW_BYTES */
static uint8_t  s_rev[256];      /* bit-reverse of a byte */

static void rev_init(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t v = (uint8_t)i, r = 0;
        for (int k = 0; k < 8; k++) {
            r = (uint8_t)((r << 1) | (v & 1u));
            v >>= 1;
        }
        s_rev[i] = r;
    }
}

/* Fast xorshift PRNG, remixed with the clock each seed so repeated reseeds
 * differ. AND'ing `sparse` draws gives a 2^-sparse live fraction. */
static uint32_t s_rng = 2463534242u;

static inline uint32_t xrand(void) {
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s_rng = x;
}

static void seed(uint32_t *g, int sparse) {
    s_rng ^= sys_millis() * 2654435761u;
    if (!s_rng) {
        s_rng = 1;
    }
    for (int i = 0; i < LIFE_WORDS; i++) {
        uint32_t w = 0xffffffffu;
        for (int k = 0; k < sparse; k++) {
            w &= xrand();
        }
        g[i] = w;
    }
}

/* Pack the 1-bit grid into the HAL mask (MSB = leftmost pixel). Our words are
 * LSB = leftmost column, so each byte's bit order is reversed; the byte order
 * (low byte = leftmost cells) already matches. */
static void pack(const uint32_t *g) {
    for (int y = 0; y < GH; y++) {
        const uint32_t *row = g + (size_t)y * WW;
        uint8_t *m = s_mask + (size_t)y * ROW_BYTES;
        for (int x = 0; x < WW; x++) {
            uint32_t w = row[x];
            m[x * 4 + 0] = s_rev[w & 0xffu];
            m[x * 4 + 1] = s_rev[(w >> 8) & 0xffu];
            m[x * 4 + 2] = s_rev[(w >> 16) & 0xffu];
            m[x * 4 + 3] = s_rev[(w >> 24) & 0xffu];
        }
    }
}

static void draw(macjr_hal_display_t *d, const uint32_t *g) {
    pack(g);
    d->blit_1bpp_mask(d, 0, 0, GW, GH, ROW_BYTES, s_mask, FG_RGB, BG_RGB);
    if (d->end_frame) {
        d->end_frame(d);
    }
}

int main(int argc, char *argv[], macjr_tty_t *tty) {
    macjr_hal_display_t *d = macjr_tty_display(tty);
    if (!d || !d->blit_1bpp_mask) {
        return 1;
    }

    int sparse = SPARSE_DEF;
    if (argc > 1 && argv[1]) {
        int v = atoi(argv[1]);
        if (v >= SPARSE_MIN && v <= SPARSE_MAX) {
            sparse = v;
        }
    }

    uint32_t *a = malloc(LIFE_WORDS * sizeof(uint32_t));
    uint32_t *b = malloc(LIFE_WORDS * sizeof(uint32_t));
    s_mask = malloc((size_t)GH * ROW_BYTES);
    if (!a || !b || !s_mask) {
        free(a);
        free(b);
        free(s_mask);
        s_mask = NULL;
        return 1;
    }

    rev_init();
    uint32_t *cur = a, *nxt = b;
    seed(cur, sparse);

    int fd = open(KBD_DEV_PATH, O_RDONLY);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
    }

    bool paused = false;
    int delay_ms = DELAY_DEF;

    if (d->fill_rect) {
        d->fill_rect(d, 0, 0, GW, GH, BG_RGB);
    }
    draw(d, cur);

    for (;;) {
        bool redraw = false;

        uint8_t key;
        while (fd >= 0 && read(fd, &key, 1) == 1) {
            if (KBD_IS_CHORD(key)) {
                continue;
            }
            switch (key) {
            case KEY_ESC:                       /* Cx: leave */
                goto done;
            case KEY_MK_ENTER:                  /* В↑: reseed */
                seed(cur, sparse);
                redraw = true;
                break;
            case KEY_MK_SP:                     /* С/П: run / pause */
                paused = !paused;
                break;
            case KEY_MK_STEP_FWD:               /* ШГ→: single step while paused */
                if (paused) {
                    life_step(cur, nxt);
                    uint32_t *t = cur; cur = nxt; nxt = t;
                    redraw = true;
                }
                break;
            case KEY_MK_PLUS:                   /* faster */
                if (delay_ms > DELAY_MIN) delay_ms -= 5;
                break;
            case KEY_MK_MINUS:                  /* slower */
                if (delay_ms < DELAY_MAX) delay_ms += 5;
                break;
            case KEY_MK_DIV:                    /* ÷: sparser soup + reseed */
                if (sparse < SPARSE_MAX) sparse++;
                seed(cur, sparse);
                redraw = true;
                break;
            case KEY_MK_MUL:                    /* ×: denser soup + reseed */
                if (sparse > SPARSE_MIN) sparse--;
                seed(cur, sparse);
                redraw = true;
                break;
            default:
                break;
            }
        }

        if (monitor_is_open()) {                /* the monitor owns the screen */
            sys_delay(50);
            continue;
        }

        if (!paused) {
            life_step(cur, nxt);
            uint32_t *t = cur; cur = nxt; nxt = t;
            redraw = true;
        } else if (!redraw) {
            sys_delay(30);
            continue;
        }

        if (redraw) {
            draw(d, cur);
        }
        if (!paused) {
            sys_delay(delay_ms);
        }
    }

done:
    if (fd >= 0) {
        close(fd);
    }
    free(a);
    free(b);
    free(s_mask);
    s_mask = NULL;
    return 0;
}
