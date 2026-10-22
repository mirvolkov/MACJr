#pragma once

/* Run the matrix-multiplication benchmark: scalar Xtensa ASM (int32) vs
 * esp-dsp SIMD path (f32). Also benches dot product and GEMV (matvec).
 * Prints results to stdout. */
void matmul_bench_run(void);
