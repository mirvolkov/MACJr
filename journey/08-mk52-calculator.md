# 8. An MK-52 inside the MK-52

With every key mapped, the board could finally become the calculator whose
case it lives in. Boot menu option 3 now starts an Elektronika MK-52.

## Finding the specification

The MK-52 user manual exists as a scanned English translation, but its key
and command tables are images, so the text alone was not enough. The
missing piece came from the MK-61 English manual, which has the full
command code table as text. The two models share one command set, so
together they gave us everything: what each key does with and without the
F and K prefixes, and the two-digit code each command stores in program
memory.

## One core, two front ends

The calculator core is plain C with no hardware code at all. Every key
press becomes the same code the real calculator would store, and one
function executes a code whether it was typed or read from a program:

```c
case 0x13: /* ÷ */
  if (x == 0.0) m->error = true; else binary(m, m->y / x);
```

Because the core has no hardware dependencies, we compiled it on the Mac
and tested it with key sequences before it ever touched the board:

```
ok    1 up 3 /                      -> [3.3333333-01]
ok    vo F ee F * F + * 4 / sp      -> [50 13 04  06]   program mode
ok    F neg 4 vo sp                 -> [12.56637]       area of a circle
```

## The calculator truncates

The manual's area-of-a-circle program says d = 4 gives `12.56637`. Ours
said `12.566371`. Two details of the real machine explain it: its π is
`3.1415926` (cut off, not rounded), and every result is *truncated* to
8 digits, not rounded. With both changes, 16 × π = 50.2654816 becomes
50.265481, and dividing by 4 gives exactly `12.56637`. The same rule
turns √2 into `1.4142135`, which matches real units.

## On screen

The digit line is drawn straight onto the display at 24x64 pixels per
character, under a plain header like the monitor's, with a one-line status
below it:

```
 Elektronika MK-52
 ────────────────────────────────────────
     -1.2345678-01
 AUTO  F  DEG
```

Drawing 64-pixel-tall digits exposed a latent bug: the display driver's
buffer holds 40 rows, and a taller call would have written past its end.
The driver now splits tall drawing calls into bands.

**Result:** a working MK-52 with RPN stack, 15 registers, 105-step
programs, loops, subroutines and indirect jumps. Still missing: the
built-in EEPROM and the bitwise operations, and the slide switches have no
pins yet (А↑ cycles the angle mode for now).
