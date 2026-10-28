# 7. Mapping the MK-52 keys

The keyboard driver numbered keys 128 to 159 by wiring position, which
says nothing about which key is "7" and which is "F". Before the board can
become an MK-52 replica, every code needs its real name.

## Learning the machine first

We started by reading up on the calculator itself: an RPN programmable
from 1985 with an 8+2 digit green VFD display, 105 program steps,
15 registers, and a 512-byte EEPROM for keeping programs. We found the
full user manual in English and the classic Russian programming book by
Трохименко. They became our specification for the calculator logic.

## Listening to the serial port

The plan was simple: the user presses each key in a known order, and we
read the debug log line for each press. The VS Code serial monitor held
the port, so it was closed, and we started our own logger that reconnects
whenever the board resets.

The first logger died on the first reset. It ran in zsh, and when the USB
port briefly disappeared, the wildcard `/dev/cu.usbmodem*` matched
nothing, which zsh treats as an error that ends the command. Running the
same loop in bash with `nullglob` fixed it. Then the presses came in one
by one:

```
kbrd: key 158      #1  F
kbrd: key 155      #2  ШГ→
kbrd: key 156      #3  П→x
...
kbrd: key 140      #32 Cx
```

All 32 codes arrived exactly once, with no gaps or repeats.

## The result

```
         col 1    col 2    col 3    col 4    col 5    col 6    col 7    col 8
 row 1   F   158  ШГ→ 155  П→x 156  7   131  8   130  9   138  −   144  ÷   149
 row 2   K   150  ←ШГ 151  x→П 157  4   137  5   135  6   136  +   142  ×   143
 row 3   ↑↓  128  В/О 153  БП  154  1   134  2   133  3   139  ↔   147  В↑  148
 row 4   А↑  129  С/П 152  ПП  159  0   132  .   145  /-/ 146  ВП  141  Cx  140
```

The table now lives in `keyboard.h`, together with a named constant for
every key, so code reads like the calculator's faceplate:

```c
if (key == KEY_MK_ENTER) push(x);        /* В↑ */
if (key == KEY_MK_F)     shift = true;   /* upper functions */
```

The boot menu and the monitor switched to these names straight away. One
thing surfaced: the monitor key turned out to be **Cx**, which the
calculator will need for clearing X, so it will move to a key combination.
