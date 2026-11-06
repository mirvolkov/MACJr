# 5. An MK-52 inside the MK-52

With [every key mapped](01-keyboard.md), the board could become the calculator
whose case it lives in: an RPN programmable from 1985 with an 8+2 digit
display, 105 program steps and 15 registers.

## Finding the specification

The MK-52 user manual exists as a scanned English translation, but its key and
command tables are images. The missing piece came from the MK-61 English
manual, which has the full command code table as text. The two models share
one command set, so together with Трохименко's classic programming book they
gave us everything: what each key does with and without the F and K prefixes,
and the code each command stores in program memory.

## One core, two front ends

The core is plain C with no hardware code. Every key press becomes the code
the real calculator would store, and one function executes a code whether it
was typed or read from a program:

```c
case 0x13: /* ÷ */
  if (x == 0.0) m->error = true; else binary(m, m->y / x);
```

So we compiled it on the Mac and tested it with key sequences before it ever
touched the board:

```
ok    1 up 3 /                      -> [3.3333333-01]
ok    vo F ee F * F + * 4 / sp      -> [50 13 04  06]   program mode
ok    F neg 4 vo sp                 -> [12.56637]       area of a circle
```

## The calculator truncates

The manual's area-of-a-circle program says d = 4 gives `12.56637`. Ours said
`12.566371`. The real machine's π is `3.1415926` (cut off, not rounded), and
every result is *truncated* to 8 digits. With both changes, 16 × π =
50.2654816 becomes 50.265481, and dividing by 4 gives exactly `12.56637`. The
same rule turns √2 into `1.4142135`, which matches real units.

**Result:** a working MK-52 with RPN stack, 15 registers, 105-step programs,
loops, subroutines and indirect jumps, now the `MK52` program with its digit
line in plain red text. Still missing: the built-in EEPROM and the bitwise
operations. The slide switches have no pins yet, so А↑ cycles the angle mode.
