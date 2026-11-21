# 16. A C compiler on the board

Until now every program was built on the Mac. Now MACJr compiles C itself:
open a `.c` file in the navigator, and `CC.ELF` turns it into a program next to
it. `hello.c` becomes `hello.elf`, and В↑ runs it.

## Which compiler

GCC can't run here: its compiler alone is tens of megabytes and wants hundreds
of megabytes of memory. A small compiler fits. **TinyCC** has a compiler,
assembler and linker in about 70,000 lines, runs in a few hundred kilobytes, and
was made to compile fast rather than well. It knows x86, ARM, ARM64 and RISC-V,
but not Xtensa. The mailing list had a few questions about an Xtensa port over
the years, and no answers. So the job was a new code generator.

## Speaking the firmware's language

The generator has to call firmware functions directly, so it must follow the
same rules as GCC. On the ESP32-S3 that is the *windowed* calling convention. A
`call8` slides a window over the 64 physical registers, so the caller's `a10`
to `a15` become the callee's `a2` to `a7`. Arguments travel there, results come
back in `a10`, and the caller's own `a0` to `a7` survive the call untouched.
The callee's `entry` instruction moves the stack pointer.

We didn't trust memory for the details. We compiled small functions with
Espressif's GCC and read the disassembly:

- where the seventh and eighth arguments go (`sp+0`, `sp+4`);
- that a `long long` starts on an even register, or moves to the stack;
- that a 36-byte struct goes on the stack;
- that a struct over 16 bytes comes back through a hidden pointer.

Every instruction encoding was checked against the GNU assembler the same way.

## Two things the chip doesn't have

**No flags.** RISC-V has none either, and TinyCC's RISC-V backend showed the
way: a compare remembers its two registers, and the branch that follows tests
them directly. One spot in the generic code still assumed flags. A `long long`
compare tests the high words, then reuses the flags to ask "were they
different?". Xtensa can't ask twice, so for this target the high words are kept
and compared again.

**No PC-relative data.** The loader puts code and data in different memory, so
the distance between them isn't known when compiling. Every address of a
function, string or global becomes a 32-bit word in the code, jumped over and
read with `L32R`, with a relocation the loader fills in. Calls are `L32R a8`
plus `CALLX8 a8`. It costs a few bytes per call and never runs out of range.

## A file the loader takes

TinyCC's own linker writes Linux shared libraries, with tables our loader
doesn't map. So TinyCC only compiles into an object in memory, and `cc` writes
the program file itself:

- `.text`, `.rodata`, `.data` and `.bss`;
- `R_XTENSA_RELATIVE` for the program's own addresses;
- `R_XTENSA_GLOB_DAT` for firmware functions, found by name at load time.

Before writing, `cc` asks the firmware's symbol table about each import. A
missing function is reported when you compile, not when you run.

TinyCC itself runs as an ordinary program from the card. It uses a thread with
a 96 KB stack in PSRAM, on the second core at idle priority, so a long compile
can't trip the watchdog.

## Tested without the chip, then with it

The same sources also build as a cross compiler on the Mac. That let us
disassemble its output, check it with the loader rules every app passes, and
compare it with GCC's. Then `tests.c` ran on the board: 48 of 51 checks passed.

The three failures were one bug. When an address is computed into a register,
TinyCC leaves an unrelated old offset in the value's offset field. The RISC-V
backend ignores that field, with a comment saying why; ours used it. Every
store through a pointer landed 44 bytes off.

To stop depending on the board for every check, we wrote a small **Xtensa
simulator** in Python. It knows only the instructions our generator emits, and
it stands in for the firmware functions programs call. It ran the board's own
`tests.elf` and failed exactly the same three checks. With the fix it passes
all 51, and now it runs on every change.

## How fast

`cc` reports what it compiled: lines including headers, compile time, write
time, and lines per second. `bench.c` runs nine integer benchmarks, each
checking its own result. The same file built by GCC `-O2` is `CCBENCH.ELF`, so
the two compilers can be compared on identical source.

## The first program built only on the board

The text viewer is the first program that exists only as source on the
card: `SRC/TEXTVIEWER/textviewer.c`. It is one file, because the navigator
hands CC one file, and it uses only the headers in `SRC/INCLUDE`. The
navigator opens text files (`.txt .md .log .ini .cfg .csv .json .h`) with
`/BIN/TEXTVIEWER.ELF` and the file's path, the way it hands pictures to the
image viewer.

```
═══════════════[long.txt]═══════════════
Line 3: the quick brown fox jumps over ▒
the lazy dog again and again           ▒
    Tabbed  line    with    tabs       ■
Привiт, свiт! Їжак, Ганок, "лапки" -   ▒
 Line 3/63 2%         -+ page  Cx exit
```

It wraps lines at word boundaries to 39 columns and draws the list
window's scroll bar in the 40th. A file that is valid UTF-8 is shown with
CP866 glyphs for Cyrillic (Ukrainian included), box drawing and
punctuation; any other file is shown as CP866. Control characters,
including ESC, show as dots, so a file can't drive the terminal.

It was tested before it ever reached the board. The Mac build of CC
compiled it, and the simulator ran it with file reads, argv, scripted keys
and a 40x15 screen added on top. That caught the wrong case: the tty has no
column-only escape (`ESC [ n G`). Then CC on the board compiled the same
source into an ELF that was byte for byte the one the Mac build made, and
that ELF went to `/BIN`.

**Result:** C programs written, compiled and run on the board, and a compiler
backend tested on the host before it reaches the chip. Not there yet: floating
point, variadic function definitions, variable-length arrays and inline
assembly.
