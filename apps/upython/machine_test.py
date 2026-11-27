# machine module check: Pin, PWM, ADC, SoftI2C, SoftSPI, Signal,
# time_pulse_us, mem32, on GPIOs the board leaves free (2, 15-18) and with
# nothing wired to them. Fits the 40x15 screen; waits for a key at the end.
import machine
from machine import Pin, PWM, ADC, SoftI2C, SoftSPI, Signal

ok = 0
bad = []

def check(name, cond, got=''):
    global ok
    if cond:
        ok += 1
    else:
        bad.append(name)
        print('BAD %s %s' % (name, got))

# Pin: out, read back, toggle, pull-up on an input
p = Pin(16, Pin.OUT, value=0)
check('out0', p.value() == 0)
p.on()
check('out1', p.value() == 1)
p.toggle()
check('toggle', p.value() == 0)
q = Pin(18, Pin.IN, Pin.PULL_UP)
check('pullup', q.value() == 1)
q.init(pull=Pin.PULL_DOWN)
check('pulldown', q.value() == 0)
print(repr(p))

# MicroPython's own pin table: flash, PSRAM, USB pins are not there
for n in (19, 20, 26, 33, 37, 47):
    try:
        Pin(n)
        check('pin%d' % n, False, 'opened')
    except ValueError:
        ok += 1

# Signal: an inverted pin
s = Signal(Pin(16, Pin.OUT), invert=True)
s.on()
check('signal', p.value() == 0)

# PWM on GPIO 15
w = PWM(Pin(15), freq=1000, duty_u16=32768)
check('pwmfreq', w.freq() == 1000, w.freq())
check('pwmduty', abs(w.duty_u16() - 32768) < 64, w.duty_u16())
w.freq(5000)
check('pwmfreq2', w.freq() == 5000, w.freq())
print(repr(w))
w.deinit()

# ADC on GPIO 2 (ADC1, floating: any value is fine)
a = ADC(Pin(2), atten=ADC.ATTN_11DB)
r = a.read_u16()
check('adc', 0 <= r <= 65535, r)
uv = a.read_uv()
check('adcuv', 0 <= uv <= 3600000, uv)
print('ADC GPIO2: %d, %d mV' % (r, uv // 1000))

# SoftI2C on 17/18: no devices there; the chip's pull-ups hold the lines
Pin(17, Pin.IN, Pin.PULL_UP)
Pin(18, Pin.IN, Pin.PULL_UP)
i2c = SoftI2C(scl=Pin(17), sda=Pin(18), freq=100000)
found = i2c.scan()
check('i2cscan', found == [], found)

# SoftSPI on 16/15/18: loopback-free, just the calls
spi = SoftSPI(baudrate=100000, sck=Pin(16), mosi=Pin(15), miso=Pin(18))
buf = bytearray(4)
spi.write_readinto(b'\x55\xaa\x0f\xf0', buf)
check('spi', len(buf) == 4)

# time_pulse_us: nothing changes on a pulled-down pin: timeout
q.init(Pin.IN, Pin.PULL_DOWN)
t = machine.time_pulse_us(q, 1, 2000)
check('pulse', t < 0, t)

# mem32: GPIO_OUT_REG (0x60004004) has the bit of a pin set high
p.init(Pin.OUT)
p.on()
check('mem32', (machine.mem32[0x60004004] >> 16) & 1 == 1)

print('machine: %d ok, %d bad' % (ok, len(bad)))
input('Enter')
