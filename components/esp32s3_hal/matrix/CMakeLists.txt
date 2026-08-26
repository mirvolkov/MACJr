# Matrix multiplication benchmark: hand-written Xtensa scalar ASM (int32)
# vs esp-dsp SIMD PIE path (f32). Build as standalone app_main or call
# matmul_i32_asm() from another component by adding "matrix" to REQUIRES.
idf_component_register(
  SRCS
    "matmul_bench.c"
    "matmul_i32.S"
  INCLUDE_DIRS "."
  REQUIRES esp_timer espressif__esp-dsp
)
