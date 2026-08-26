#include "matmul_bench.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "esp_timer.h"
#include "dspm_mult.h"

extern void     matmul_i32_asm(int32_t *C, const int32_t *A, const int32_t *B, int32_t n);
extern void     matvec_i32_asm(int32_t *y, const int32_t *W, const int32_t *x,
                                const int32_t *b, int32_t rows, int32_t cols);
extern int32_t  dot_i32_asm(const int32_t *a, const int32_t *b, int32_t n);

#define N       64          /* matmul square size (bumped from 32) */
#define MV_ROWS 64          /* matvec output dimension             */
#define MV_COLS 256         /* matvec / dot-product vector length  */
#define ITERS   50

/* --- matmul buffers --- */
static int32_t A_i32[N * N];
static int32_t B_i32[N * N];
static int32_t C_i32[N * N];

static float A_f32[N * N];
static float B_f32[N * N];
static float C_f32[N * N];

/* --- matvec / dot buffers --- */
static int32_t W_i32[MV_ROWS * MV_COLS];
static int32_t x_i32[MV_COLS];
static int32_t bias_i32[MV_ROWS];
static int32_t y_i32[MV_ROWS];

static void fill_all(void)
{
    for (int i = 0; i < N * N; i++) {
        int v = (rand() % 21) - 10;
        A_i32[i] = v;  B_i32[i] = -v;
        A_f32[i] = (float)v;  B_f32[i] = (float)-v;
    }
    for (int i = 0; i < MV_ROWS * MV_COLS; i++)
        W_i32[i] = (rand() % 21) - 10;
    for (int i = 0; i < MV_COLS; i++)
        x_i32[i] = (rand() % 21) - 10;
    for (int i = 0; i < MV_ROWS; i++)
        bias_i32[i] = rand() % 100;
}

/* C reference dot product */
static int32_t dot_c(const int32_t *a, const int32_t *b, int32_t n)
{
    int32_t acc = 0;
    for (int i = 0; i < n; i++) acc += a[i] * b[i];
    return acc;
}

/* C reference matvec with bias */
static void matvec_c(int32_t *y, const int32_t *W, const int32_t *x,
                     const int32_t *b, int32_t rows, int32_t cols)
{
    for (int i = 0; i < rows; i++) {
        int32_t acc = 0;
        for (int k = 0; k < cols; k++) acc += W[i * cols + k] * x[k];
        y[i] = acc + (b ? b[i] : 0);
    }
}

void matmul_bench_run(void)
{
    fill_all();

    int64_t t0;

    /* ── dot product ────────────────────────────────────────────── */
    volatile int32_t sink;
    dot_c(x_i32, x_i32, MV_COLS);                              // warm-up
    t0 = esp_timer_get_time();
    for (int it = 0; it < ITERS; it++)
        sink = dot_c(x_i32, x_i32, MV_COLS);
    int64_t t_dot_c_us = esp_timer_get_time() - t0;

    dot_i32_asm(x_i32, x_i32, MV_COLS);                        // warm-up
    t0 = esp_timer_get_time();
    for (int it = 0; it < ITERS; it++)
        sink = dot_i32_asm(x_i32, x_i32, MV_COLS);
    int64_t t_dot_asm_us = esp_timer_get_time() - t0;
    (void)sink;

    /* ── GEMV / matvec ──────────────────────────────────────────── */
    matvec_c(y_i32, W_i32, x_i32, bias_i32, MV_ROWS, MV_COLS); // warm-up
    t0 = esp_timer_get_time();
    for (int it = 0; it < ITERS; it++)
        matvec_c(y_i32, W_i32, x_i32, bias_i32, MV_ROWS, MV_COLS);
    int64_t t_mv_c_us = esp_timer_get_time() - t0;

    matvec_i32_asm(y_i32, W_i32, x_i32, bias_i32, MV_ROWS, MV_COLS); // warm-up
    t0 = esp_timer_get_time();
    for (int it = 0; it < ITERS; it++)
        matvec_i32_asm(y_i32, W_i32, x_i32, bias_i32, MV_ROWS, MV_COLS);
    int64_t t_mv_asm_us = esp_timer_get_time() - t0;

    /* ── matmul NxN ─────────────────────────────────────────────── */
    matmul_i32_asm(C_i32, A_i32, B_i32, N);                    // warm-up
    t0 = esp_timer_get_time();
    for (int it = 0; it < ITERS; it++)
        matmul_i32_asm(C_i32, A_i32, B_i32, N);
    int64_t t_asm_us = esp_timer_get_time() - t0;

    dspm_mult_f32(A_f32, B_f32, C_f32, N, N, N);               // warm-up
    t0 = esp_timer_get_time();
    for (int it = 0; it < ITERS; it++)
        dspm_mult_f32(A_f32, B_f32, C_f32, N, N, N);
    int64_t t_simd_us = esp_timer_get_time() - t0;

    /* ── print results ──────────────────────────────────────────── */
    double dot_macs  = MV_COLS;
    double mv_macs   = (double)MV_ROWS * MV_COLS;
    double mm_macs   = (double)N * N * N;

    printf("\n=== dot product  len=%d, %d iters ===\n", MV_COLS, ITERS);
    printf("  C scalar : %6.2f us/call   %6.1f MMAC/s\n",
           (double)t_dot_c_us  / ITERS, dot_macs / ((double)t_dot_c_us  / ITERS));
    printf("  ASM      : %6.2f us/call   %6.1f MMAC/s   speedup %.2fx\n",
           (double)t_dot_asm_us / ITERS, dot_macs / ((double)t_dot_asm_us / ITERS),
           (double)t_dot_c_us / t_dot_asm_us);

    printf("\n=== GEMV (matvec)  %dx%d + bias, %d iters ===\n",
           MV_ROWS, MV_COLS, ITERS);
    printf("  C scalar : %6.2f us/call   %6.1f MMAC/s\n",
           (double)t_mv_c_us  / ITERS, mv_macs / ((double)t_mv_c_us  / ITERS));
    printf("  ASM      : %6.2f us/call   %6.1f MMAC/s   speedup %.2fx\n",
           (double)t_mv_asm_us / ITERS, mv_macs / ((double)t_mv_asm_us / ITERS),
           (double)t_mv_c_us / t_mv_asm_us);

    printf("\n=== matmul  %dx%d, %d iters ===\n", N, N, ITERS);
    printf("  ASM scalar int32 : %8.1f us/call   %6.1f MMAC/s\n",
           (double)t_asm_us  / ITERS, mm_macs / ((double)t_asm_us  / ITERS));
    printf("  esp-dsp SIMD f32 : %8.1f us/call   %6.1f MMAC/s\n",
           (double)t_simd_us / ITERS, mm_macs / ((double)t_simd_us / ITERS));
    printf("  speedup (simd/asm): %.2fx\n\n",
           (double)t_asm_us / t_simd_us);
}
