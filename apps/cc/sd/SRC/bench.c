/*
 * bench.c -- integer benchmarks: how fast the code a compiler makes runs.
 *
 * Open it in the navigator and cc compiles it (its report shows how long
 * that took); run bench.elf for the times. The same source is built by GCC
 * -O2 as /bin/CCBENCH.ELF (apps/ccbench), to compare the two compilers.
 *
 * Each benchmark ends with a checksum of what it computed; "ok" means it
 * matches the one the program was checked against, so a wrong result
 * shows up as "BAD" rather than as a fast time.
 *
 *   ═════════════[Bench: tcc]══════════════
 *   sieve        412 ms  ok
 *   ...
 */
#include <stdio.h>
#include <string.h>
#include <macjr.h>

#ifdef __TINYC__
#define COMPILER "tcc"
#else
#define COMPILER "gcc"
#endif

typedef unsigned int u32;
typedef unsigned long long u64;

/* ------------------------------------------------------------------------- */
/* the screen */

static macjr_tty_t *s_tty;

static void print(const char *s)
{
    macjr_tty_write(s_tty, s, strlen(s));
}

static void flush(void)
{
    macjr_display_flush(s_tty);
}

/* a pseudo-random sequence that every compiler computes the same way */
static u32 s_seed;

static u32 lcg(void)
{
    s_seed = s_seed * 1103515245u + 12345u;
    return s_seed >> 8;
}

/* ------------------------------------------------------------------------- */
/* 1. sieve of Eratosthenes: loops and byte stores */

#define SIEVE_N    8192
#define SIEVE_REPS 12

static unsigned char s_flags[SIEVE_N + 1];

static u32 bench_sieve(void)
{
    int rep, i, k, count = 0;
    for (rep = 0; rep < SIEVE_REPS; rep++) {
        count = 0;
        for (i = 2; i <= SIEVE_N; i++)
            s_flags[i] = 1;
        for (i = 2; i <= SIEVE_N; i++) {
            if (s_flags[i]) {
                count++;
                for (k = i + i; k <= SIEVE_N; k += i)
                    s_flags[k] = 0;
            }
        }
    }
    return count;
}

/* ------------------------------------------------------------------------- */
/* 2. CRC-32: table lookups, shifts, xor */

#define CRC_BYTES 16384
#define CRC_REPS  10

static u32 s_crc_table[256];
static unsigned char s_data[CRC_BYTES];

static void crc_init(void)
{
    u32 n, c;
    int k;
    for (n = 0; n < 256; n++) {
        c = n;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        s_crc_table[n] = c;
    }
}

static u32 crc32(const unsigned char *p, int len, u32 crc)
{
    crc = ~crc;
    while (len--)
        crc = s_crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static u32 bench_crc(void)
{
    int i, rep;
    u32 crc = 0;
    crc_init();
    s_seed = 1;
    for (i = 0; i < CRC_BYTES; i++)
        s_data[i] = lcg();
    for (rep = 0; rep < CRC_REPS; rep++)
        crc = crc32(s_data, CRC_BYTES, crc);
    return crc;
}

/* ------------------------------------------------------------------------- */
/* 3. quicksort: recursion, array indexing, compares */

#define SORT_N 12000

static int s_arr[SORT_N];

static void quicksort(int *a, int lo, int hi)
{
    while (lo < hi) {
        int pivot = a[(lo + hi) >> 1], i = lo, j = hi, t;
        while (i <= j) {
            while (a[i] < pivot)
                i++;
            while (a[j] > pivot)
                j--;
            if (i <= j) {
                t = a[i];
                a[i] = a[j];
                a[j] = t;
                i++;
                j--;
            }
        }
        /* recurse into the smaller half, loop on the bigger one */
        if (j - lo < hi - i) {
            quicksort(a, lo, j);
            lo = i;
        } else {
            quicksort(a, i, hi);
            hi = j;
        }
    }
}

static u32 bench_sort(void)
{
    int i;
    u32 sum = 0;
    s_seed = 42;
    for (i = 0; i < SORT_N; i++)
        s_arr[i] = (int)(lcg() % 100000) - 50000;
    quicksort(s_arr, 0, SORT_N - 1);
    for (i = 0; i < SORT_N; i++) {
        if (i && s_arr[i - 1] > s_arr[i])
            return 0; /* not sorted */
        sum = sum * 31 + (u32)s_arr[i];
    }
    return sum;
}

/* ------------------------------------------------------------------------- */
/* 4. matrix multiply: nested loops, two-dimensional arrays, multiplies */

#define MAT_N    24
#define MAT_REPS 10

static int s_ma[MAT_N][MAT_N], s_mb[MAT_N][MAT_N], s_mc[MAT_N][MAT_N];

static u32 bench_matrix(void)
{
    int i, j, k, rep;
    u32 sum = 0;
    s_seed = 7;
    for (i = 0; i < MAT_N; i++)
        for (j = 0; j < MAT_N; j++) {
            s_ma[i][j] = (int)(lcg() & 255) - 128;
            s_mb[i][j] = (int)(lcg() & 255) - 128;
        }
    for (rep = 0; rep < MAT_REPS; rep++)
        for (i = 0; i < MAT_N; i++)
            for (j = 0; j < MAT_N; j++) {
                int acc = 0;
                for (k = 0; k < MAT_N; k++)
                    acc += s_ma[i][k] * s_mb[k][j];
                s_mc[i][j] = acc;
            }
    for (i = 0; i < MAT_N; i++)
        for (j = 0; j < MAT_N; j++)
            sum = sum * 17 + (u32)s_mc[i][j];
    return sum;
}

/* ------------------------------------------------------------------------- */
/* 5. eight queens: backtracking */

#define QUEENS 8

static int s_col[QUEENS];

static int safe(int row, int col)
{
    int r, d;
    for (r = 0; r < row; r++) {
        d = s_col[r] - col;
        if (d == 0 || d == row - r || d == r - row)
            return 0;
    }
    return 1;
}

static int place(int row)
{
    int col, n = 0;
    if (row == QUEENS)
        return 1;
    for (col = 0; col < QUEENS; col++) {
        if (safe(row, col)) {
            s_col[row] = col;
            n += place(row + 1);
        }
    }
    return n;
}

static u32 bench_queens(void)
{
    return place(0) + place(0) * 1000;
}

/* ------------------------------------------------------------------------- */
/* 6. Fibonacci, the slow way: calls and returns */

static int fib(int n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static u32 bench_fib(void)
{
    return fib(24);
}

/* ------------------------------------------------------------------------- */
/* 7. strings: the firmware's string functions and snprintf */

#define STR_ITEMS 400
#define STR_REPS  6

static char s_names[STR_ITEMS][16];

static u32 bench_strings(void)
{
    int i, j, rep;
    u32 sum = 0;
    char tmp[32];
    for (rep = 0; rep < STR_REPS; rep++) {
        for (i = 0; i < STR_ITEMS; i++)
            snprintf(s_names[i], sizeof(s_names[i]), "item%d-%x", i * 7 + rep, i * 13);
        for (i = 0; i < STR_ITEMS; i += 4) {
            for (j = 0; j < STR_ITEMS; j += 8)
                if (strcmp(s_names[i], s_names[j]) < 0)
                    sum++;
            strcpy(tmp, s_names[i]);
            strcat(tmp, "!");
            sum = sum * 3 + strlen(tmp);
            memmove(tmp + 1, tmp, strlen(tmp) + 1);
            sum += (unsigned char)tmp[2];
        }
    }
    return sum;
}

/* ------------------------------------------------------------------------- */
/* 8. 64-bit arithmetic: xorshift and the libgcc helpers */

#define XS_REPS 30000

static u32 bench_long(void)
{
    u64 x = 0x9e3779b97f4a7c15ull, acc = 0;
    int i;
    for (i = 0; i < XS_REPS; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        acc += x % 1000003;
        acc ^= (u64)(i * 2654435761u) << 3;
    }
    return (u32)(acc ^ (acc >> 32));
}

/* ------------------------------------------------------------------------- */
/* 9. a little stack machine: switch dispatch */

enum { OP_PUSH, OP_LOAD, OP_STORE, OP_ADD, OP_SUB, OP_MUL, OP_LT, OP_JZ, OP_JMP, OP_HALT };

/* for (i = 0; i < 20000; i++) s = s + i * i - 7;  (variables 0: i, 1: s) */
static const int s_prog[] = {
    OP_PUSH, 0, OP_STORE, 0,
    OP_PUSH, 0, OP_STORE, 1,
    /* 8: loop */
    OP_LOAD, 0, OP_PUSH, 20000, OP_LT, OP_JZ, 37,
    OP_LOAD, 1, OP_LOAD, 0, OP_LOAD, 0, OP_MUL, OP_ADD, OP_PUSH, 7, OP_SUB, OP_STORE, 1,
    OP_LOAD, 0, OP_PUSH, 1, OP_ADD, OP_STORE, 0,
    OP_JMP, 8,
    /* 37: */
    OP_HALT,
};

static u32 run_vm(const int *code)
{
    int stack[16], vars[4], sp = 0, pc = 0;
    vars[0] = vars[1] = 0;
    for (;;) {
        switch (code[pc++]) {
        case OP_PUSH:
            stack[sp++] = code[pc++];
            break;
        case OP_LOAD:
            stack[sp++] = vars[code[pc++]];
            break;
        case OP_STORE:
            vars[code[pc++]] = stack[--sp];
            break;
        case OP_ADD:
            sp--;
            stack[sp - 1] += stack[sp];
            break;
        case OP_SUB:
            sp--;
            stack[sp - 1] -= stack[sp];
            break;
        case OP_MUL:
            sp--;
            stack[sp - 1] *= stack[sp];
            break;
        case OP_LT:
            sp--;
            stack[sp - 1] = stack[sp - 1] < stack[sp];
            break;
        case OP_JZ:
            if (stack[--sp] == 0)
                pc = code[pc];
            else
                pc++;
            break;
        case OP_JMP:
            pc = code[pc];
            break;
        case OP_HALT:
            return (u32)vars[1];
        default:
            return 0;
        }
    }
}

static u32 bench_vm(void)
{
    return run_vm(s_prog);
}

/* ------------------------------------------------------------------------- */
/* the table */

typedef struct {
    const char *name;
    u32 (*run)(void);
    u32 expect; /* checksum of a correct run */
} bench_t;

static const bench_t s_benches[] = {
    {"sieve", bench_sieve, 0x00000404},
    {"crc32", bench_crc, 0x1efe82bf},
    {"quicksort", bench_sort, 0x8c24b8c3},
    {"matrix", bench_matrix, 0xf525bbb9},
    {"queens", bench_queens, 0x000167bc},
    {"fib(24)", bench_fib, 0x0000b520},
    {"strings", bench_strings, 0x5fb82f76},
    {"long long", bench_long, 0x088c7073},
    {"stack vm", bench_vm, 0xd5ca52d0},
};

#define N_BENCH (int)(sizeof(s_benches) / sizeof(s_benches[0]))

int main(int argc, char *argv[], macjr_tty_t *tty)
{
    char line[64];
    unsigned total = 0;
    int i, bad = 0;

    s_tty = tty;
    print("\x1b[?25l\x1b[0m\x1b[2J");
    macjr_tty_header(tty, "Bench: " COMPILER);
    for (i = 0; i < N_BENCH; i++) {
        unsigned t0, ms;
        u32 sum;
        snprintf(line, sizeof(line), "\x1b[%d;1H\x1b[97m%-11s\x1b[90m...", 3 + i, s_benches[i].name);
        print(line);
        flush();
        t0 = sys_millis();
        sum = s_benches[i].run();
        ms = sys_millis() - t0;
        total += ms;
        if (sum != s_benches[i].expect)
            bad++;
        snprintf(line, sizeof(line), "\x1b[%d;12H\x1b[96m%6u ms  %s", 3 + i, ms,
                 sum == s_benches[i].expect ? "\x1b[92mok " : "\x1b[91mBAD");
        print(line);
        if (sum != s_benches[i].expect) {
            snprintf(line, sizeof(line), " \x1b[90m%08x", sum);
            print(line);
        }
        flush();
    }
    snprintf(line, sizeof(line), "\x1b[13;1H\x1b[97mtotal      \x1b[96m%6u ms  %s", total,
             bad ? "\x1b[91mwrong results" : "\x1b[92mall correct");
    print(line);
    print("\x1b[15;1H\x1b[90m any key: exit");
    flush();
    sys_getkey();
    return bad;
}
