# 1. The keyboard

The MK-52 keyboard is a matrix of 32 keys. We connected it to an MCP23018
I/O expander on the I2C bus, so the ESP32 only needs two wires (SDA 8,
SCL 9) to read every key.

## Scanning

A row is "driven" by switching its pin to output, where it pulls low; every
other pin stays an input with a pull-up. One 2-byte I2C read returns both
ports, and any column that reads low is a pressed key in that row. A change
only counts after two scans agree, which is our debounce.

The first run crashed straight away:

```
assert failed: xTaskDelayUntil ... (xTimeIncrement > 0U)
```

We had asked for a scan every 5 ms, but the FreeRTOS tick was 10 ms, so
5 ms rounded down to zero ticks. The fix was to never wait less than one
tick.

## Two dead rows

The MK-52 matrix is not a neat grid: row pin 1, for example, only connects
to column pins 5 and 6. Once the pin map was in, keys on rows 12 and 14
worked, but nothing on rows 1 and 13 did. Swapping them in software changed
nothing.

So we wrote a wiring probe: drive each expander pin low in turn and report
every other pin that follows it. The log showed no connection at all on
those two pins. The fault was in the wiring. After rewiring them to A1 and
A0, all 32 keys answered.

## A device file

The keyboard became a Unix-style character device. Each key press is one
byte, with codes from 128 so they never collide with ASCII:

```c
int fd = open("/dev/kbrd", O_RDONLY);
uint8_t key;
read(fd, &key, 1);            /* blocks until a key is pressed */
```

Every `open()` gets its own queue, so several readers each see every key.
`O_NONBLOCK` makes `read()` return `EAGAIN`. The file lives only in memory:
ESP-IDF's VFS routes the path straight to our driver.

`select()` came later. A VFS driver supports it through two callbacks:
`start_select` clears the caller's descriptor sets and remembers what was
asked for, and `end_select` forgets the call. When a key arrives, the
keyboard task adds the descriptor back to the read set and calls
`esp_vfs_select_triggered()`. A queue per `open()` plus these two callbacks
is now the pattern for any new device.

The queues have a limit. After the kill work, WIFI stopped reacting to keys:
`/dev/kbrd` had room for 4 readers, and the firmware alone held all 4 (key
beep, monitor, kill watch, `sys_getkey()`). The limit is 8 now, and running
out logs a warning.

## Naming the keys

The driver numbered keys 128 to 159 by wiring position, which says nothing
about which key is "7" and which is "F". So the keys were pressed in a known
order while a logger read the serial port. The first logger died on the
first reset: in zsh, `/dev/cu.usbmodem*` matching nothing is an error that
ends the command. The same loop in bash with `nullglob` worked, and all 32
codes arrived exactly once:

```
         col 1    col 2    col 3    col 4    col 5    col 6    col 7    col 8
 row 1   F   158  ШГ→ 155  П→x 156  7   131  8   130  9   138  −   144  ÷   149
 row 2   K   150  ←ШГ 151  x→П 157  4   137  5   135  6   136  +   142  ×   143
 row 3   ↑↓  128  В/О 153  БП  154  1   134  2   133  3   139  ↔   147  В↑  148
 row 4   А↑  129  С/П 152  ПП  159  0   132  .   145  /-/ 146  ВП  141  Cx  140
```

The table lives in `keyboard.h` with a named constant for every key, so code
reads like the faceplate: `if (key == KEY_MK_ENTER) push(x);`.

## ESC and chords

Cx became ESC: the keyboard sends ASCII 27, and every program treats it as
"back". The calculator is the exception, since it needs Cx to clear X.

A scan drives one row and reads every column, so two keys held together both
show up. The matrix has no diodes, so three keys on the corners of a
rectangle would show a phantom fourth, but a pair is always read correctly.

The hard part of a chord is the first key: F has already been pressed when K
arrives, and sending it at once would leave a stray F in the program. So keys
that can start a chord wait. The partner turns the pair into one code, a
release lets the key through alone, and any other key sends it first.

| Chord | Action |
|---|---|
| F + K | System monitor |
| F + Cx | Kill the program |
| F + ↑↓ | Screenshot |

Chords belong to the system, and `KBD_IS_CHORD()` lets programs skip them.

**Result:** a debounced 32-key keyboard any program can read like a file or
wait on with `select()`, and a wiring probe to reuse the next time something
stays silent.
