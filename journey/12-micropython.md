# 12. A Python on the calculator

MACJr got its first programming language beyond MK-52 keystrokes: MicroPython,
built as an ordinary program, `UPYTHON.ELF`, and loaded like any other from
`/BIN`.

## An interpreter as a program

MicroPython has an "embed" port for exactly this: it turns the interpreter into
a plain folder of C files that another build compiles. The upstream tree is a
git submodule in `vendor/micropython`. `apps/upython/Makefile.embed` generates
`micropython_embed/` from it, and the apps Makefile cross-compiles that like
any other program. The port itself is three small files:

- `mpconfigport.h` picks the features.
- `mphalport.c` writes to the tty and turns MK-52 keys into characters.
- `main.c` is the program.

The first build found the usual seams between two build systems:

- The generator preprocesses the port headers on the Mac, which has no
  firmware headers, so the HAL header now declares the two calls it uses
  itself.
- macOS ships make 3.81, which takes the *first* pattern rule that matches,
  not the most specific. MicroPython's sources were compiled as an app until
  their rule moved above the generic one.
- The import checker asked for `memcmp`, `strncmp`, `strncpy` and
  `nearbyint`, now exported. The garbage collector was off, so `gc_init`
  was missing too.

## ESP_ERR_NO_MEM

The first launch failed before the program started. To make room for
MicroPython, the program stack had been raised from 12 KB to 48 KB. FreeRTOS
takes a stack from internal SRAM in one piece, and with Wi-Fi running there
was no free 48 KB block. Every program, not just Python, failed to start.

The big stack came from a 32 KB REPL buffer kept on it. That buffer moved to
static memory, which the loader places in PSRAM. The stack went to 16 KB,
the size MicroPython's own ESP32 port runs in. MicroPython's stack check is
on, so deep recursion raises `RuntimeError` instead of overrunning the stack.

## The exception that crashed the board

The demo script then rebooted the board:

```
Guru Meditation Error: Core 1 panic'ed (LoadProhibited)
EXCVADDR: 0x00000138
```

The ELF is stripped, so we relinked a copy with symbols, worked out the load
address from the entry point, and decoded the backtrace:

```
app_main → mp_embed_exec_str → mp_obj_print_exception
         → mp_obj_is_exception_instance → mp_obj_get_type
```

The crash came while *printing* an exception: the exception object itself was
garbage. MicroPython raises exceptions with a non-local return, `nlr`, and on
Xtensa it picks a hand-written assembly version. That version is for the
ESP8266's call0 ABI. The S3 uses register windows, so every `raise` came back
with a broken object. MicroPython's own ESP32 port sets `MICROPY_NLR_SETJMP`,
and so does ours now. The exception it was trying to print was a plain
`SyntaxError`: the demo used f-strings, which are off at the BASIC feature
level.

## Time and a benchmark

Turning `time` on in the config built nothing, because the embed package
copies only `py/`. `Makefile.embed` now has an `EMBED_EXTMOD` list: each
module there joins MicroPython's string scan and is copied into the package.
`ticks_us()` comes from `esp_timer`. The demo now times 1000 `math.sin()`
calls and a Simpson's-rule integral of sin from 0 to π, which must come out
as 2.0000000000.

## Scripts as files

The first version had its own menu of `/PY`. Now the navigator does that
job. `.py` files show in green, and В↑ on one runs `UPYTHON.ELF <file>`: the
firmware's `exec_launch_with()` passes the path as `argv[1]`. With no
argument the program starts the REPL.

- In the REPL, a line that opens a block continues on `...` lines.
- An expression's value is printed, as in any Python REPL.
- Cx on an empty line exits.
- A script exits with code 1 if it ends with an exception, after printing
  the traceback with its file name.

Typing uses the MK-52 keys: digits and operators direct, punctuation after F,
letters a-z after K. There is no `<`, `>` or capital letter yet.

**Result:** MicroPython 1.29 as a 194 KB program, a REPL and a script runner
started from the navigator, and two lessons for the next big port: check
which assembly a library picks for this CPU, and keep big buffers off the
stack.
