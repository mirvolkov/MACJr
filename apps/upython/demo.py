import math
import time

def header(s):
    w = 38
    pad = (w - len(s)) // 2
    print('=' * pad + ' ' + s + ' ' + '=' * (w - pad - len(s) - 2))

header('MACJr Python')
print()

# Primes up to 50 with a simple sieve
N = 50
sieve = [True] * N
sieve[0] = sieve[1] = False
for i in range(2, 8):
    for j in range(i * i, N, i):
        sieve[j] = False
primes = [i for i in range(N) if sieve[i]]
print('Primes to 50:')
print(primes)
print()

# Fibonacci sequence
print('Fibonacci:')
a, b, row = 0, 1, []
for _ in range(11):
    row.append(a)
    a, b = b, a + b
print(row)
print()

# Trig table (like the MK-52 manual examples)
print('Trig (deg):')
for deg in [0, 30, 45, 60, 90]:
    r = deg * math.pi / 180
    s = math.sin(r)
    c = math.cos(r)
    print(f'  {deg:2}  sin={s:.4f}  cos={c:.4f}')

print()

# Benchmark: time some float-heavy work
print('Benchmark:')

t = time.ticks_us()
s = 0.0
for i in range(1000):
    s += math.sin(i * 0.001)
dt = time.ticks_diff(time.ticks_us(), t)
print(f'  1000 sin   {dt / 1000:7.1f} ms')
print(f'  sum={s:.6f}  {dt / 1000:.1f} us/call')

# Simpson's rule: integral of sin, 0..pi = 2
n = 10000
h = math.pi / n
t = time.ticks_ms()
acc = math.sin(0) + math.sin(math.pi)
for i in range(1, n):
    acc += (4 if i % 2 else 2) * math.sin(i * h)
dt = time.ticks_diff(time.ticks_ms(), t)
print(f'  Simpson n={n}  {dt:5} ms')
print(f'  integral={acc * h / 3:.10f}')

print()
header('done')
