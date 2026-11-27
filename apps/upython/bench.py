# VM benchmark for the assembly fast path (vm_xtensa.S), which is always
# on. Each test is timed and its result checked against what CPython
# gives (EXPECT, computed on a PC); then the edge cases of integer
# arithmetic (small ints, overflow into long ints, mixed types) are
# checked the same way, by a checksum. Fits the 40x15 screen.
#
# Before the fast path became permanent, the same tests ran with it off
# and on (vmasm()): C 2464 ms, asm 1188 ms in total, 2.07x.
import time

def t_loop(n):
    i = 0
    while i < n:
        i += 1
    return i

def t_range(n):
    s = 0
    for i in range(n):
        s += i & 255
    return s

def t_fib(n):
    return n if n < 2 else t_fib(n - 1) + t_fib(n - 2)

def t_collatz(n):
    total = 0
    for k in range(1, n):
        x = k
        while x != 1:
            if x % 2:
                x = 3 * x + 1
            else:
                x = x // 2
            total += 1
    return total

def t_gcd(n):
    s = 0
    for a in range(1, n):
        b = 1000003 % a
        x = a
        while b:
            x, b = b, x % b
        s += x
    return s

def t_bits(n):
    c = 0
    for k in range(n):
        x = k
        while x:
            c += x & 1
            x >>= 1
    return c

def t_sieve(n):
    s = [True] * n
    i = 2
    while i * i < n:
        if s[i]:
            j = i * i
            while j < n:
                s[j] = False
                j += i
        i += 1
    return sum(s)

def t_float(n):
    s = 0.0
    x = 0.5
    for i in range(n):
        s += x * x
        x += 0.001
    return int(s)

TESTS = (
    ('loop', t_loop, 60000),
    ('range', t_range, 60000),
    ('fib', t_fib, 18),
    ('collatz', t_collatz, 600),
    ('gcd', t_gcd, 3000),
    ('bits', t_bits, 6000),
    ('sieve', t_sieve, 20000),
    ('float', t_float, 20000),
)

def run(f, n):
    t = time.ticks_ms()
    r = f(n)
    return time.ticks_diff(time.ticks_ms(), t), r

# ---- edge cases: every op on a set of values, results or exception names

# The small ints are -2**30 .. 2**30-1: the values around the edges, so
# results spill into long ints. (The bottom one is written as a sum: a
# build without long ints could not parse the literal -1073741824, which
# is -(1073741824), one past the top.)
VALS = (0, 1, -1, 2, -3, 7, -8, 31, 40, 12345, -54321,
        1073741823, -1073741823 - 1, 1073741822, -1073741823, 46340, -46341)
SHIFTS = (0, 1, 5, 29, 30, 31, 32, 40, -1)

def ex(e):
    return type(e).__name__

class Ck:
    """Count and checksum of the results' text as they come (no list to
    keep: 4000 results in one list need a 32 KB block of the heap). The
    same in MicroPython and CPython."""
    def __init__(self):
        self.n = 0
        self.h = 0
    def add(self, r):
        h = self.h
        for c in str(r):
            h = (h * 31 + ord(c)) & 0x3FFFFFFF
        self.h = h
        self.n += 1

def edge():
    ck = Ck()
    ap = ck.add
    for a in VALS:
        try:
            ap((-a, ~a, +a, not a, a == 0))
        except Exception as e:
            ap(ex(e))
        if a:
            ap('t')
        for b in VALS:
            for k in range(14):
                try:
                    if k == 0: r = a + b
                    elif k == 1: r = a - b
                    elif k == 2: r = a * b
                    elif k == 3: r = a // b
                    elif k == 4: r = a % b
                    elif k == 5: r = a & b
                    elif k == 6: r = a | b
                    elif k == 7: r = a ^ b
                    elif k == 8: r = (a < b, a <= b, a > b, a >= b)
                    elif k == 9: r = (a == b, a != b, a is b)
                    elif k == 10:
                        x = a
                        x += b
                        x -= 3
                        x *= 2
                        r = x
                    elif k == 11: r = a and b
                    elif k == 12: r = a or b
                    else: r = (a < b) is (b > a)
                except Exception as e:
                    r = ex(e)
                ap(r)
        for s in SHIFTS:
            try:
                r = a << s
            except Exception as e:
                r = ex(e)
            ap(r)
            try:
                r = a >> s
            except Exception as e:
                r = ex(e)
            ap(r)
    # mixed types: must fall back to C and still be right
    for a, b in ((1, 2.5), (2.5, 1), (True, 1), (1, None), ('a', 'b'), (None, None)):
        for k in range(4):
            try:
                if k == 0: r = a + b
                elif k == 1: r = a < b
                elif k == 2: r = a == b
                else: r = a * b
            except Exception as e:
                r = ex(e)
            ap(r)
    return ck.n, ck.h

# what CPython gives (computed on a PC); edge: (count, checksum)
EXPECT = {'loop': 60000, 'range': 7642320, 'fib': 2584, 'collatz': 32494, 'gcd': 2999, 'bits': 36656, 'sieve': 2264, 'float': 2871456, 'edge': (4409, 626379344)}

def main():
    print('test       ms   result')
    total = 0
    bad = []
    for name, f, n in TESTS:
        d, r = run(f, n)
        total += d
        ok = EXPECT is None or EXPECT[name] == r
        if not ok:
            bad.append(name)
        print('%-8s %6d   %s' % (name, d, 'ok' if ok else 'BAD %s' % r))
    print('%-8s %6d' % ('total', total))
    got = edge()
    if EXPECT is None or got == EXPECT['edge']:
        print('edge: OK, %d results' % got[0])
    else:
        print('edge: BAD %d %08x' % got)
        bad.append('edge')
    if bad:
        print('BAD:', ' '.join(bad))
    input('2**100 = %d' % (2 ** 100))  # wait: the navigator would redraw

main()
