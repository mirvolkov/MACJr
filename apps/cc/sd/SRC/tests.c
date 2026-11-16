/* tests.c -- checks the code cc generates: build it (open it in the
 * navigator), then run tests.elf. Each line is a group of checks; a
 * failing check shows its number. */
#include <stdio.h>
#include <string.h>
#include <macjr.h>

static macjr_tty_t *s_tty;
static int s_fails, s_checks, s_group_fails;
static char s_line[48];

static void print(const char *s)
{
    macjr_tty_write(s_tty, s, strlen(s));
}

static void check(int ok, int n)
{
    s_checks++;
    if (!ok) {
        s_fails++;
        if (s_group_fails++ < 3) {
            snprintf(s_line, sizeof(s_line), " #%d", n);
            print(s_line);
        }
    }
}

static void group(const char *name)
{
    s_group_fails = 0;
    snprintf(s_line, sizeof(s_line), "\r\n\x1b[97m%-10s\x1b[91m", name);
    print(s_line);
}

/* --- integers --- */
static int add(int a, int b) { return a + b; }
static unsigned udiv(unsigned a, unsigned b) { return a / b; }
static int sdiv(int a, int b) { return a / b; }

static void t_int(void)
{
    volatile int a = 7, b = -3, z = 0;
    volatile unsigned u = 0xfffffff0u;
    group("int");
    check(a + b == 4, 1);
    check(a - b == 10, 2);
    check(a * b == -21, 3);
    check(sdiv(a, b) == -2, 4);
    check(a % b == 1, 5);
    check(udiv(u, 16) == 0x0fffffff, 6);
    check(u % 7 == 0xfffffff0u % 7, 7);
    check((a & 3) == 3 && (a | 8) == 15 && (a ^ 5) == 2, 8);
    check((a << 4) == 112 && (b >> 1) == -2 && (u >> 4) == 0x0fffffff, 9);
    check((u >> 20) == 0xfff && (u << 31) == 0, 10);
    check(-a == -7 && ~a == -8 && !z && !!a, 11);
    check(add(1000000, 2345678) == 3345678, 12);
    check(0x12345678 + a == 0x1234567f, 13);
    check(z - 100000 == -100000, 14);
}

/* --- compares and branches --- */
static void t_cmp(void)
{
    volatile int a = 5, b = -5, z = 0;
    volatile unsigned ua = 5, ub = 0xfffffffbu;
    int n = 0, i;
    group("compare");
    check(a > b && b < a && a >= 5 && a <= 5 && a != b && !(a == b), 1);
    check(ua < ub && ub > ua && ub >= ua && ua <= ub, 2);
    check(z == 0 && !(z != 0) && !(z < 0) && z >= 0 && z <= 0 && !(z > 0), 3);
    check(b < 0 && !(b >= 0) && a > 0 && !(a <= 0), 4);
    check((a > b) == 1 && (a < b) == 0, 5);
    for (i = 0; i < 10; i++)
        n += i;
    check(n == 45, 6);
    switch (a) {
    case 1: n = 1; break;
    case 5: n = 5; break;
    default: n = 0;
    }
    check(n == 5, 7);
    check((a && b) && (a || z) && !(z && a), 8);
    n = a > 3 ? 10 : 20;
    check(n == 10, 9);
}

/* --- long long --- */
static long long lmul(long long a, long long b) { return a * b; }
static unsigned long long lshl(unsigned long long a, int n) { return a << n; }

static void t_llong(void)
{
    volatile long long a = 0x100000000LL, b = -1;
    volatile unsigned long long u = 0xffffffffull;
    volatile int sh = 36;
    group("long long");
    check(a + b == 0xffffffffLL, 1);
    check(a - 1 == 0xffffffffLL, 2);
    check(u + 1 == 0x100000000ull, 3);
    check(lmul(a, 3) == 0x300000000LL, 4);
    check(lmul(-7, 6) == -42, 5);
    check(a / 2 == 0x80000000LL && a % 3 == 1, 6);
    check(lshl(1, sh) == 0x1000000000ull && (a >> 1) == 0x80000000LL, 7);
    check(a > b && b < a && u < (unsigned long long)a && a != u, 8);
    check((int)(a >> 32) == 1 && (unsigned)u == 0xffffffffu, 9);
    check(b == -1 && (a & u) == 0 && (a | u) == 0x1ffffffffLL, 10);
    check(u * u == 0xfffffffe00000001ull, 11);
}

/* --- memory: chars, shorts, arrays, structs, globals --- */
struct point { int x, y; };
struct big { int v[8]; char c; };
static struct point s_pts[3] = {{1, 2}, {3, 4}, {5, 6}};
static const char *s_names[] = {"zero", "one", "two"};
static int s_counter = 42;
static char s_text[] = "macjr";

static struct point mid(struct point a, struct point b)
{
    struct point m;
    m.x = (a.x + b.x) / 2;
    m.y = (a.y + b.y) / 2;
    return m;
}

static int sum_big(struct big b)
{
    int i, s = b.c;
    for (i = 0; i < 8; i++)
        s += b.v[i];
    return s;
}

static struct big make_big(int base)
{
    struct big b;
    int i;
    for (i = 0; i < 8; i++)
        b.v[i] = base + i;
    b.c = 1;
    return b;
}

static void t_mem(void)
{
    volatile signed char sc = -2;
    volatile unsigned char uc = 254;
    volatile short ss = -3;
    volatile unsigned short us = 65533;
    char buf[16];
    int arr[10], i, s = 0;
    struct point p;
    struct big bg;
    group("memory");
    check(sc == -2 && uc == 254 && ss == -3 && us == 65533, 1);
    check((int)(signed char)uc == -2 && (unsigned char)sc == 254, 2);
    for (i = 0; i < 10; i++)
        arr[i] = i * i;
    for (i = 0; i < 10; i++)
        s += arr[i];
    check(s == 285, 3);
    check(s_pts[2].y == 6 && strcmp(s_names[1], "one") == 0, 4);
    check(s_counter == 42 && strlen(s_text) == 5 && s_text[2] == 'c', 5);
    s_counter++;
    check(s_counter == 43, 6);
    p = mid(s_pts[0], s_pts[2]);
    check(p.x == 3 && p.y == 4, 7);
    bg = make_big(10);
    check(bg.v[7] == 17 && bg.c == 1, 8);
    check(sum_big(bg) == 10 * 8 + 28 + 1, 9);
    strcpy(buf, "abc");
    check(buf[3] == 0 && strlen(buf) == 3, 10);
    {
        int *ip = &arr[3];
        check(*ip == 9 && ip[1] == 16 && *(ip - 1) == 4 && ip - arr == 3, 11);
    }
}

/* --- calls: many arguments, function pointers, recursion --- */
static int eight(int a, int b, int c, int d, int e, int f, int g, int h)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}

static long long mixed(int a, long long b, int c, long long d)
{
    return a + b + c + d;
}

static int fib(int n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static int twice(int (*f)(int, int), int x)
{
    return f(x, x);
}

static void t_call(void)
{
    char buf[24];
    group("calls");
    check(eight(1, 2, 3, 4, 5, 6, 7, 8) == 204, 1);
    check(mixed(1, 0x100000000LL, 2, 3) == 0x100000006LL, 2);
    check(fib(15) == 610, 3);
    check(twice(add, 21) == 42, 4);
    snprintf(buf, sizeof(buf), "%d %s %x", -12, "ok", 255);
    check(strcmp(buf, "-12 ok ff") == 0, 5);
    snprintf(buf, sizeof(buf), "%lld", 1234567890123LL);
    check(strcmp(buf, "1234567890123") == 0, 6);
}

int main(int argc, char *argv[], macjr_tty_t *tty)
{
    s_tty = tty;
    print("\x1b[0m\x1b[2J");
    macjr_tty_header(tty, "cc tests");
    print("\x1b[2;1H");
    t_int();
    t_cmp();
    t_llong();
    t_mem();
    t_call();
    snprintf(s_line, sizeof(s_line), "\r\n\r\n%s%d of %d checks passed",
             s_fails ? "\x1b[91m" : "\x1b[92m", s_checks - s_fails, s_checks);
    print(s_line);
    print("\x1b[15;1H\x1b[90m any key: exit");
    macjr_display_flush(tty);
    sys_getkey();
    return s_fails != 0;
}
