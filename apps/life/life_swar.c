/*
 * Bit-parallel Game of Life -- see life_swar.h.
 *
 * Each cell's next state is a 3x3 all-ones convolution (the neighbour count)
 * plus a threshold. Packed one bit per cell, we can't use '+' -- a carry would
 * leak from one cell's bit into its neighbour's. So the count is built from
 * XOR/AND full-adders that keep every cell's arithmetic inside its own bit
 * column, advancing 32 cells per word.
 *
 * Including the centre cell, the 3x3 sum S is 0..9 and the rule collapses to
 *   next = (S == 3) || (S == 4 && self)
 * (dead+3 -> born; live+2 -> S=3 survive; live+3 -> S=4 & self survive).
 */
#include "life_swar.h"

#include <stddef.h>

/* Horizontal neighbours within a row, carrying the boundary bit across words:
 * bit k of LSH is the cell to its left (column-1), of RSH the one to its right. */
#define LSH(r, prev) (((r) << 1) | ((prev) >> 31))
#define RSH(r, next) (((r) >> 1) | ((next) << 31))

/* self + left + right of one row -> 2-bit count (s1,s0), per cell 0..3. */
#define ROW3(l, c, r, s0, s1)                        \
    do {                                             \
        uint32_t _l = (l), _c = (c), _r = (r);       \
        (s0) = _l ^ _c ^ _r;                         \
        (s1) = (_l & _c) | (_l & _r) | (_c & _r);    \
    } while (0)

void life_step(const uint32_t *cur, uint32_t *nxt) {
    for (int y = 0; y < LIFE_GH; y++) {
        const uint32_t *M = cur + (size_t)y * LIFE_WW;
        const uint32_t *U = cur + (size_t)((y == 0 ? LIFE_GH : y) - 1) * LIFE_WW;     /* row above, wrap */
        const uint32_t *D = cur + (size_t)((y + 1 == LIFE_GH) ? 0 : y + 1) * LIFE_WW; /* row below, wrap */
        uint32_t *N = nxt + (size_t)y * LIFE_WW;

        for (int x = 0; x < LIFE_WW; x++) {
            int xl = (x == 0) ? LIFE_WW - 1 : x - 1;           /* word to the left,  wrap */
            int xr = (x + 1 == LIFE_WW) ? 0 : x + 1;           /* word to the right, wrap */

            uint32_t um = U[x], mm = M[x], dm = D[x];
            uint32_t ul = LSH(um, U[xl]), ur = RSH(um, U[xr]);
            uint32_t ml = LSH(mm, M[xl]), mr = RSH(mm, M[xr]);
            uint32_t dl = LSH(dm, D[xl]), dr = RSH(dm, D[xr]);

            uint32_t t0, t1, m0, m1, b0, b1;
            ROW3(ul, um, ur, t0, t1);
            ROW3(ml, mm, mr, m0, m1);   /* the middle row's count includes the centre cell */
            ROW3(dl, dm, dr, b0, b1);

            /* add the three 2-bit numbers -> S (0..9) as bits S3..S0 */
            uint32_t low      = t0 ^ m0 ^ b0;
            uint32_t carryLow = (t0 & m0) | (t0 & b0) | (m0 & b0);
            uint32_t p0 = t1 ^ m1,       p1 = t1 & m1;
            uint32_t q0 = b1 ^ carryLow, q1 = b1 & carryLow;
            uint32_t S0 = low;
            uint32_t S1 = p0 ^ q0;
            uint32_t c4 = p0 & q0;
            uint32_t S2 = p1 ^ q1 ^ c4;
            uint32_t S3 = (p1 & q1) | (p1 & c4) | (q1 & c4);

            /* next = (S == 3) | (S == 4 & self) */
            uint32_t eq3 = ~S3 & ~S2 &  S1 &  S0;   /* 0011 */
            uint32_t eq4 = ~S3 &  S2 & ~S1 & ~S0;   /* 0100 */
            N[x] = eq3 | (eq4 & mm);
        }
    }
}
