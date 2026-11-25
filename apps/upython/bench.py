# VM benchmark: the C VM alone vs the assembly fast path (vm_xtensa.S).
# Every test runs twice, vmasm(False) then vmasm(True); both must give
# the same result. Then the edge cases of small-int arithmetic, compared
# the same way. Fits the 40x15 screen.
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

VALS = (0, 1, -1, 2, -3, 7, -8, 31, 40, 12345, -54321,
        1073741823, -1073741824, 1073741822, -1073741823, 46340, -46341)
SHIFTS = (0, 1, 5, 29, 30, 31, 32, 40, -1)

def ex(e):
    return type(e).__name__

def edge():
    out = []
    ap = out.append
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
    return out

def main():
    print('test     C ms  asm ms   gain')
    tc = ta = 0
    bad = []
    for name, f, n in TESTS:
        vmasm(False)
        dc, rc = run(f, n)
        vmasm(True)
        da, ra = run(f, n)
        tc += dc
        ta += da
        g = dc / da if da else 0
        mark = '' if rc == ra else ' BAD'
        if mark:
            bad.append(name)
        print('%-7s %6d %7d %5.2fx%s' % (name, dc, da, g, mark))
    print('%-7s %6d %7d %5.2fx' % ('total', tc, ta, tc / ta if ta else 0))
    vmasm(False)
    ec = edge()
    vmasm(True)
    ea = edge()
    diff = [i for i in range(len(ec)) if ec[i] != ea[i]]
    if diff or len(ec) != len(ea):
        i = diff[0] if diff else 0
        print('edge: BAD %d of %d' % (len(diff), len(ec)))
        print(' #%d C=%s' % (i, str(ec[i])[:30]))
        print(' asm=%s' % str(ea[i])[:34])
    else:
        print('edge: OK, %d results' % len(ec))
    if bad:
        print('BAD:', ' '.join(bad))

main()
