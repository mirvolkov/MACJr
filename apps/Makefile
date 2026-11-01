# MacJr programs for the SD card.
#
# Every directory here with .c files is one program: apps/<name>/*.c is
# built into build/<NAME>.ELF, a position-independent ELF that the firmware
# loads from /BIN on the SD card (components/exec, espressif/elf_loader).
# Programs see the firmware's headers with its build flags, so build the
# firmware (idf.py build) before them.
#
#   make                          build all programs
#   make install CARD=/Volumes/X  copy them to X/BIN
#   make clean
#
# Needs the ESP-IDF environment (export.sh) for the Xtensa toolchain.

FIRMWARE_BUILD ?= ../build
CROSS          ?= xtensa-esp32s3-elf-
CC             := $(CROSS)gcc
STRIP          := $(CROSS)strip
OUT            := build

lower = $(shell echo $(1) | tr A-Z a-z)
upper = $(shell echo $(1) | tr a-z A-Z)

APPS := $(sort $(patsubst %/,%,$(dir $(wildcard */*.c))))
ELFS := $(foreach a,$(APPS),$(OUT)/$(call upper,$(a)).ELF)

# Objects of program NAME (directory name in lower case)
app_objs = $(patsubst %.c,$(OUT)/obj/%.o,$(wildcard $(call lower,$(1))/*.c))

# Include dirs, defines and toolchain options of the firmware build
FW_FLAGS := $(shell python3 tools/appflags.py $(FIRMWARE_BUILD)/compile_commands.json)

CFLAGS := $(FW_FLAGS) -I../sdk/include \
          -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
          -ffunction-sections -fdata-sections -fvisibility=hidden -fPIC \
          -Dmain=app_main

# What espressif/elf_loader's project_elf() does: a shared object without
# startup files or libc (those come from the firmware), entry app_main.
# libgcc is linked in for the compiler's helpers (double arithmetic,
# 64-bit division etc.), from a copy without .eh_frame: the loader doesn't
# map unwind tables, and their relocations would make it reject the image.
LDFLAGS := -nostartfiles -nostdlib -shared -e app_main \
           -Wl,--gc-sections -Wl,--strip-all -Wl,--strip-debug -Wl,--strip-discarded
LIBGCC  := $(OUT)/libgcc.a
LDLIBS  := $(LIBGCC)

# What the firmware exports to programs: checked against every program's
# imports (tools/checkelf.py, with the loader's other rules), so a program
# the loader would reject fails here and not on the board.
EXPORTS := ../components/exec/exports.c ../managed_components/espressif__elf_loader/src/esp_elf_symbol.c

STRIPFLAGS := --strip-unneeded --remove-section=.comment --remove-section=.got.loc \
              --remove-section=.dynamic --remove-section=.xt.lit \
              --remove-section=.xt.prop --remove-section=.xtensa.info

all: $(ELFS)

.SECONDEXPANSION:

# Compile and link are separate steps: the firmware flags carry the
# picolibc specs, which must not reach the shared-object link.
$(OUT)/obj/%.o: %.c $$(wildcard $$(dir $$*)*.h) ../sdk/include/macjr.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# $(OUT)/NAME.ELF from the sources of the directory name (lower case)
$(LIBGCC):
	@mkdir -p $(OUT)
	$(CROSS)objcopy --remove-section=.eh_frame --remove-section=.rela.eh_frame $$($(CC) -print-libgcc-file-name) $@

$(OUT)/%.ELF: $$(call app_objs,$$*) $(LIBGCC) $(EXPORTS)
	$(CC) $(LDFLAGS) $(filter %.o,$^) $(LDLIBS) -o $@
	$(STRIP) $(STRIPFLAGS) $@
	@python3 tools/checkelf.py $(CROSS)readelf $@ $(EXPORTS) || { rm -f $@; exit 1; }
	@echo "$@: $$(wc -c < $@) bytes"

install: all
	@test -n "$(CARD)" || { echo "usage: make install CARD=/Volumes/<card>"; exit 1; }
	mkdir -p "$(CARD)/BIN"
	cp $(ELFS) "$(CARD)/BIN/"
	-command -v dot_clean >/dev/null && dot_clean -m "$(CARD)/BIN" # macOS ._ metadata files

clean:
	rm -rf $(OUT)

.PHONY: all install clean

# keep the objects for incremental rebuilds
.SECONDARY:
