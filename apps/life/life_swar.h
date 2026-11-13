#ifndef MACJR_LIFE_SWAR_H
#define MACJR_LIFE_SWAR_H

#include <stdint.h>

/*
 * Bit-parallel ("SIMD within a register") Game of Life.
 *
 * The grid is LIFE_GH rows of LIFE_WW 32-bit words, one bit per cell, bit 0 =
 * the leftmost column of each word (column = word_index*32 + bit). Edges wrap
 * (toroidal). One generation advances 32 cells per word: the eight neighbours
 * are summed with bitwise full-adders -- no per-cell branches, no lookup.
 */
#define LIFE_GW 320                 /* columns (a multiple of 32)   */
#define LIFE_GH 240                 /* rows                         */
#define LIFE_WW (LIFE_GW / 32)      /* words per row (10)           */
#define LIFE_WORDS (LIFE_GH * LIFE_WW)

/* Advance one generation: cur -> nxt (each LIFE_WORDS words, distinct). */
void life_step(const uint32_t *cur, uint32_t *nxt);

#endif
