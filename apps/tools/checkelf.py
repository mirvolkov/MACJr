#!/usr/bin/env python3
"""Fail if the firmware's loader would reject a program.

The loader (espressif/elf_loader) only reports problems on the board, as
"Can't find symbol" or "Invalid ELF image". This checks the same rules at
build time:

  - imports: every symbol the program imports is exported by the firmware
    (components/exec/exports.c or the loader's built-in libc table);
  - relocations: only the types the Xtensa loader applies (no thread-local
    data such as errno, no R_XTENSA_NONE left over from dropped sections);
  - sections: everything the program needs at run time is in a section
    the loader maps (.text, .rodata, .data, .data.rel.ro, .bss, .got).

usage: checkelf.py <readelf> <program.ELF> <exports.c> <esp_elf_symbol.c>
"""
import re
import subprocess
import sys

RELOCS = {"R_XTENSA_RELATIVE", "R_XTENSA_RTLD", "R_XTENSA_GLOB_DAT", "R_XTENSA_JMP_SLOT"}
# allocated sections the loader maps, or reads for linking
SECTIONS = {".text", ".rodata", ".data", ".data.rel.ro", ".bss", ".got",
            ".hash", ".dynsym", ".dynstr", ".rela.dyn", ".rela.plt"}


def readelf(tool, *args):
    # stderr "no .dynamic section" is expected: the build strips .dynamic
    return subprocess.run([tool, "-W", *args], capture_output=True, text=True).stdout


def exported(paths):
    names = set()
    for p in paths:
        with open(p) as f:
            names |= set(re.findall(r"ESP_ELFSYM_EXPORT\((\w+)\)", f.read()))
    return names


def imported(tool, elf):
    names = set()
    for line in readelf(tool, "--dyn-syms", elf).splitlines():
        cols = line.split()
        if len(cols) >= 8 and cols[6] == "UND" and cols[7]:
            names.add(cols[7])
    return names


def relocs(tool, elf):
    return set(re.findall(r"\bR_XTENSA_\w+", readelf(tool, "-r", elf)))


def alloc_sections(tool, elf):
    names = set()
    for line in readelf(tool, "-S", elf).splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(\S*A\S*)\s", line)
        if m:
            names.add(m.group(1))
    return names


def main() -> int:
    tool, elf, *tables = sys.argv[1:]
    errors = []
    missing = sorted(imported(tool, elf) - exported(tables))
    if missing:
        errors.append("the firmware doesn't export: %s (add to components/exec/exports.c)" % " ".join(missing))
    bad = sorted(relocs(tool, elf) - RELOCS)
    if bad:
        errors.append("relocations the loader can't apply: %s%s" % (
            " ".join(bad), " (thread-local data, e.g. errno)" if any("TLS" in r for r in bad) else ""))
    extra = sorted(alloc_sections(tool, elf) - SECTIONS)
    if extra:
        errors.append("sections the loader doesn't map: %s" % " ".join(extra))
    for e in errors:
        print("%s: %s" % (elf, e))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
