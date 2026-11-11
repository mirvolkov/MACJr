/*
 * SDR.ELF -- a 2.4 GHz spectrogram.
 *
 * Uses the firmware's espsdr capture HAL (the ESP32-S3 Wi-Fi-band receiver's
 * ADC-dump engine): each frame it snapshots raw IQ, runs an averaged
 * periodogram, and paints a scrolling waterfall plus a spectrum graph on the
 * display HAL (like the PNG viewer, apps/port). Cx exits.
 *
 * Span: 16 MHz centred on the tuned channel (2412 MHz by default),
 * 62.5 kHz/bin. The radio is taken from normal Wi-Fi while this runs.
 */
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "macjr.h"
#include "keyboard.h"
#include "monitor.h"
#include "espsdr.h"

#define NFFT   256
#define AVG    16
#define SNAP   (NFFT * AVG) /* IQ words per frame */
#define CENTER 2412u        /* MHz */

#define W      320
#define SPEC_H 48
#define WF_Y   SPEC_H
#define WF_H   (240 - SPEC_H) /* 192 */

static float  s_re[NFFT], s_im[NFFT];
static double s_win[NFFT];
static double s_acc[NFFT];
static uint8_t s_pal[256][3];
static uint8_t s_idx[W];
static float  s_floor;
static bool   s_floor_set;

/* in-place radix-2 FFT (double twiddles, float data). */
static void fft(float *re, float *im, int n) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = a + len / 2;
                float xr = re[b] * cr - im[b] * ci;
                float xi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

/* 0..255 -> RGB: black-blue-cyan-green-yellow-red-white. */
static void pal888(int v, uint8_t *o) {
    int r, g, b;
    if (v < 32)       { r = 0;            g = 0;                 b = v * 4; }
    else if (v < 96)  { r = 0;            g = (v - 32) * 4;      b = 128 + (v - 32) * 2; }
    else if (v < 160) { r = (v - 96) * 4; g = 255;              b = 255 - (v - 96) * 4; }
    else if (v < 224) { r = 255;          g = 255 - (v - 160) * 4; b = 0; }
    else              { r = 255;          g = (v - 224) * 4;    b = (v - 224) * 4; }
    o[0] = r > 255 ? 255 : r;
    o[1] = g > 255 ? 255 : g;
    o[2] = b > 255 ? 255 : b;
}

static void unpack(uint32_t w, int *i, int *q) {
    *i = ((int32_t)(w << 22)) >> 22; /* bits 0..9 */
    *q = ((int32_t)(w << 12)) >> 22; /* bits 10..19 */
}

/* One averaged periodogram from `iq` into s_idx (W palette indices). */
static void analyze(const uint32_t *iq) {
    for (int k = 0; k < NFFT; k++) s_acc[k] = 0.0;
    for (int j = 0; j < AVG; j++) {
        const uint32_t *blk = iq + (size_t)j * NFFT;
        for (int k = 0; k < NFFT; k++) {
            int i, q;
            unpack(blk[k], &i, &q);
            s_re[k] = (float)(i * s_win[k]);
            s_im[k] = (float)(q * s_win[k]);
        }
        fft(s_re, s_im, NFFT);
        for (int k = 0; k < NFFT; k++) {
            int src = (k + NFFT / 2) & (NFFT - 1); /* fftshift: DC centred */
            s_acc[k] += (double)s_re[src] * s_re[src] + (double)s_im[src] * s_im[src];
        }
    }
    float fmin = 1e30f;
    for (int k = 0; k < NFFT; k++) {
        float d = (float)(10.0 * log10(s_acc[k] / AVG + 1e-3));
        s_acc[k] = d;
        if (d < fmin) fmin = d;
    }
    if (!s_floor_set) { s_floor = fmin; s_floor_set = true; }
    else s_floor += 0.10f * (fmin - s_floor);
    float scale = 255.0f / 40.0f; /* 40 dB across the palette */
    for (int x = 0; x < W; x++) {
        float fb = (float)x * (NFFT - 1) / (W - 1);
        int b0 = (int)fb;
        int b1 = b0 < NFFT - 1 ? b0 + 1 : b0;
        float d = (float)s_acc[b0] + (fb - b0) * (float)(s_acc[b1] - s_acc[b0]);
        int v = (int)((d - s_floor) * scale);
        s_idx[x] = v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
    }
}

int main(int argc, char *argv[], macjr_tty_t *tty) {
    (void)argc;
    (void)argv;
    macjr_hal_display_t *d = macjr_tty_display(tty);
    if (!d || !d->blit_rgb888) {
        return 1;
    }

    uint32_t *iq = malloc(SNAP * sizeof(uint32_t));
    uint8_t *wf = malloc((size_t)WF_H * W * 3);  /* RGB888 waterfall (PSRAM) */
    uint8_t *sp = malloc((size_t)SPEC_H * W * 3); /* RGB888 spectrum graph */
    if (!iq || !wf || !sp) {
        free(iq); free(wf); free(sp);
        return 1;
    }
    memset(wf, 0, (size_t)WF_H * W * 3);
    for (int k = 0; k < NFFT; k++) s_win[k] = 0.5 * (1.0 - cos(2.0 * M_PI * k / (NFFT - 1)));
    for (int v = 0; v < 256; v++) pal888(v, s_pal[v]);
    if (d->fill_rect) d->fill_rect(d, 0, 0, W, 240, 0); /* black the screen */

    espsdr_start();
    espsdr_tune(CENTER);

    int fd = open(KBD_DEV_PATH, O_RDONLY);
    if (fd >= 0) fcntl(fd, F_SETFL, O_NONBLOCK);

    for (;;) {
        uint8_t key;
        if (fd >= 0 && read(fd, &key, 1) == 1 && !KBD_IS_CHORD(key)) {
            if (key == SYS_KEY_ESC) break;
        }
        if (monitor_is_open()) { /* the monitor owns the screen */
            sys_delay(50);
            continue;
        }
        if (espsdr_snapshot(iq, SNAP, ESPSDR_RATE_16M) < 0) {
            continue;
        }
        analyze(iq);

        /* waterfall: scroll down one row, new row on top, then blit it all */
        memmove(wf + (size_t)W * 3, wf, (size_t)(WF_H - 1) * W * 3);
        for (int x = 0; x < W; x++) pal888(s_idx[x], wf + (size_t)x * 3);
        d->blit_rgb888(d, 0, WF_Y, W, WF_H, wf);

        /* spectrum graph: column height from the newest row */
        memset(sp, 0, (size_t)SPEC_H * W * 3);
        for (int x = 0; x < W; x++) {
            int h = s_idx[x] * SPEC_H / 256;
            for (int y = 0; y < h; y++) {
                memcpy(sp + ((size_t)(SPEC_H - 1 - y) * W + x) * 3, s_pal[s_idx[x]], 3);
            }
        }
        d->blit_rgb888(d, 0, 0, W, SPEC_H, sp);
        if (d->end_frame) d->end_frame(d);
    }

    espsdr_stop();
    if (fd >= 0) close(fd);
    free(iq);
    free(wf);
    free(sp);
    return 0;
}
