# 1. The keyboard matrix

The MK-52 keyboard is a matrix of 32 keys. We connected it to an MCP23018
I/O expander on the I2C bus, so the ESP32 only needs two wires (SDA 8,
SCL 9) to read every key.

## Scanning

Scanning works one row at a time. A row is "driven" by switching its pin to
output, where it pulls low; every other pin stays an input with a pull-up.
One 2-byte I2C read then returns both ports at once, and any column that
reads low is a pressed key in that row. A key change only counts after two
scans agree, which is our debounce.

The first run crashed straight away:

```
assert failed: xTaskDelayUntil ... (xTimeIncrement > 0U)
```

We had asked for a scan every 5 ms, but the FreeRTOS tick was 10 ms, so
5 ms rounded down to zero ticks. The fix was to never wait less than one
tick:

```c
#define KBD_SCAN_TICKS (pdMS_TO_TICKS(KBD_SCAN_MS) > 0 ? pdMS_TO_TICKS(KBD_SCAN_MS) : 1)
```

## Two dead rows

The MK-52 matrix is not a neat grid. Its pins are numbered 1 to 14, and
row pin 1, for example, only connects to column pins 5 and 6. Once the pin
map was in, keys on rows 12 and 14 worked, but nothing on rows 1 and 13
did. Swapping them in software changed nothing.

So we wrote a wiring probe: drive each expander pin low in turn and report
every other pin that follows it. The log showed no connection at all on
the two pins used by rows 1 and 13. The fault was in the wiring, not the
code. After rewiring those rows to pins A1 and A0, all 32 keys answered.

## A device file

Polling a scan loop from every program would be messy, so the keyboard
became a Unix-style character device. Each key press is one byte, with
codes from 128 so they never collide with ASCII:

```c
int fd = open("/dev/kbrd", O_RDONLY);
uint8_t key;
read(fd, &key, 1);            /* blocks until a key is pressed */
```

Every `open()` gets its own queue, so several readers each see every key.
`O_NONBLOCK` makes `read()` return `EAGAIN` instead of waiting. The file
lives only in memory: ESP-IDF's VFS routes the path straight to our driver.

**Result:** a debounced 32-key keyboard any program can read like a file,
and a wiring probe we can reuse the next time something stays silent.
