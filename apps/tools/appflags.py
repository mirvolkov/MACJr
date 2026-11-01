#!/usr/bin/env python3
"""Print the compiler flags an app needs to see the firmware's headers.

Programs on the SD card include the same headers as the firmware (HAL,
tty, ESP-IDF, FreeRTOS, picolibc), so they are compiled with the include
dirs, defines and toolchain options of the firmware build. They are taken
from the command that compiled main/main.c (main depends on every
component) in the firmware's compile_commands.json.

usage: appflags.py <firmware build dir>/compile_commands.json
"""
import json
import shlex
import sys


def main() -> int:
    with open(sys.argv[1]) as f:
        commands = json.load(f)
    entry = next((c for c in commands if c["file"].endswith("/main/main.c")), None)
    if entry is None:
        sys.exit("main/main.c not in %s: build the firmware first" % sys.argv[1])

    args = shlex.split(entry["command"])
    keep = []
    i = 1  # args[0] is the compiler
    while i < len(args):
        a = args[i]
        if a in ("-I", "-D", "-isystem"):
            keep += [a, args[i + 1]]
            i += 1
        elif a.startswith(("-I", "-D", "-isystem", "-std=", "@")):
            keep.append(a)
        i += 1
    print(" ".join(shlex.quote(a) for a in keep))
    return 0


if __name__ == "__main__":
    sys.exit(main())
