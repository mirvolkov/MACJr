# MacJr programs for the SD card.
#
# Every directory here with .c files is one program: apps/<name>/*.c is
# built into build/<NAME>.ELF, a position-independent ELF that the firmware
# loads from /bin, the board's flash partition for programs (components/exec,
# espressif/elf_loader).
# Programs see the firmware's headers with its build flags, so build the
# firmware (idf.py build) before them.
#
#   make                          build all programs
#   make install CARD=/Volumes/X  copy the cc headers and examples to X/SRC
#   make deploy HOST=<address>    send firmware + programs over Wi-Fi
#   make flash-apps PORT=<port>   write all programs over the USB cable
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

# upython has extra sources (the micropython_embed package); handle it
# separately so the generic pattern rule doesn't apply to it.
APPS := $(sort $(filter-out upython,$(patsubst %/,%,$(dir $(wildcard */*.c)))))
ELFS := $(foreach a,$(APPS),$(OUT)/$(call upper,$(a)).ELF)

# upython: include in the build only if the embed package has been generated.
# To generate it once: cd apps/upython && make -f Makefile.embed
EMBED_DIR      := upython/micropython_embed
EMBED_SENTINEL := $(EMBED_DIR)/py/objstr.c
ifneq ($(wildcard $(EMBED_SENTINEL)),)
  ELFS += $(OUT)/UPYTHON.ELF
else
  $(info upython: embed package not found — skipping UPYTHON.ELF)
  $(info         Run: cd apps/upython && make -f Makefile.embed)
endif

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

# The MicroPython package sources, with their own flags (EMBED_CFLAGS,
# below). This rule must come before the generic one: make 3.81 (macOS)
# takes the first pattern rule that matches, not the most specific.
$(OUT)/obj/upython/micropython_embed/%.o: upython/micropython_embed/%.c
	@mkdir -p $(dir $@)
	$(CC) $(EMBED_CFLAGS) -c $< -o $@

# Compile and link are separate steps: the firmware flags carry the
# picolibc specs, which must not reach the shared-object link.
$(OUT)/obj/%.o: %.c $$(wildcard $$(dir $$*)*.h) ../sdk/include/macjr.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# cc: TinyCC (vendor/tinycc) is compiled into cc.c as one source, with its
# own style, so its warnings are off.
TINYCC := ../vendor/tinycc
$(OUT)/obj/cc/cc.o: CFLAGS += -w -I$(TINYCC)
$(OUT)/obj/cc/cc.o: $(wildcard $(TINYCC)/*.c $(TINYCC)/*.h)

# ccbench: the cc benchmark built by GCC, for comparison. -fno-builtin: it
# calls the same firmware string functions as the cc build (GCC would
# otherwise turn strcpy + strcat into stpcpy, which the firmware lacks).
$(OUT)/obj/ccbench/ccbench.o: CFLAGS += -fno-builtin
$(OUT)/obj/ccbench/ccbench.o: cc/sd/SRC/bench.c

# $(OUT)/NAME.ELF from the sources of the directory name (lower case)
$(LIBGCC):
	@mkdir -p $(OUT)
	$(CROSS)objcopy --remove-section=.eh_frame --remove-section=.rela.eh_frame $$($(CC) -print-libgcc-file-name) $@

$(OUT)/%.ELF: $$(call app_objs,$$*) $(LIBGCC) $(EXPORTS)
	$(CC) $(LDFLAGS) $(filter %.o,$^) $(LDLIBS) -o $@
	$(STRIP) $(STRIPFLAGS) $@
	@python3 tools/checkelf.py $(CROSS)readelf $@ $(EXPORTS) || { rm -f $@; exit 1; }
	@echo "$@: $$(wc -c < $@) bytes"

# ---- upython: MicroPython REPL ----
#
# The embed package (micropython_embed/) is compiled with its own flags:
#   - no firmware defines (avoid ESP-IDF macro clashes with C99 MicroPython)
#   - -fno-math-errno so libm never touches errno
#   - warnings relaxed (upstream code has intentional patterns)
#
# The app glue (main.c, mphalport.c) uses the standard firmware CFLAGS.

EMBED_CFLAGS := -std=c99 -O2 \
  -Wall -Wno-unused-variable -Wno-sign-compare \
  -Wno-missing-field-initializers -Wno-unused-function \
  -ffunction-sections -fdata-sections -fvisibility=hidden -fPIC \
  -fno-math-errno \
  -I upython \
  -I $(EMBED_DIR) \
  -I $(EMBED_DIR)/port

UPHY_SRCS  := upython/main.c upython/mphalport.c
UPHY_OBJS  := $(patsubst %.c,$(OUT)/obj/%.o,$(UPHY_SRCS))
# port/mphalport.c is the package's printf stub; upython/mphalport.c
# replaces it with the tty.
EMBED_SRCS := $(filter-out $(EMBED_DIR)/port/mphalport.c,$(wildcard $(EMBED_DIR)/*/*.c $(EMBED_DIR)/*/*/*.c))
EMBED_OBJS := $(patsubst %.c,$(OUT)/obj/%.o,$(EMBED_SRCS))

# upython glue: compiled with firmware flags + embed includes
$(OUT)/obj/upython/main.o: upython/main.c $(EMBED_SENTINEL) ../sdk/include/macjr.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I upython -I $(EMBED_DIR) -I $(EMBED_DIR)/port \
	      -Wno-unused-function -c $< -o $@

$(OUT)/obj/upython/mphalport.o: upython/mphalport.c $(EMBED_SENTINEL) ../sdk/include/macjr.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I upython -I $(EMBED_DIR) -I $(EMBED_DIR)/port \
	      -Wno-unused-function -c $< -o $@

$(OUT)/UPYTHON.ELF: $(UPHY_OBJS) $(EMBED_OBJS) $(LIBGCC) $(EXPORTS)
	$(CC) $(LDFLAGS) $(filter %.o,$^) $(LDLIBS) -o $@
	$(STRIP) $(STRIPFLAGS) $@
	@python3 tools/checkelf.py $(CROSS)readelf $@ $(EXPORTS) || { rm -f $@; exit 1; }
	@echo "$@: $$(wc -c < $@) bytes"

# Over Wi-Fi (the board on Wi-Fi: F + БП): the firmware if it changed
# (../build/MACJr.bin; the board restarts into it), then the programs that
# changed, onto the board's flash. tools/deploy.py --help for the rest.
#   make deploy HOST=192.168.1.50 [TOKEN=...]
deploy: all
	@test -n "$(HOST)" || { echo "usage: make deploy HOST=<board address> [TOKEN=...]"; exit 1; }
	python3 tools/deploy.py $(HOST) $(ELFS) $(if $(TOKEN),--token $(TOKEN))

# Over the USB cable: an image of the whole apps flash partition with every
# program, written with esptool (replaces what the partition held). Needs
# the ESP-IDF Python (esptool); ESPPY points to it.
#   make flash-apps PORT=/dev/cu.usbmodem1101
ESPPY ?= $(HOME)/.espressif/tools/python/v6.0.1/venv/bin/python
flash-apps: all
	@test -n "$(PORT)" || { echo "usage: make flash-apps PORT=<serial port>"; exit 1; }
	$(ESPPY) tools/flashapps.py --port $(PORT) $(ELFS)

install: all
	@test -n "$(CARD)" || { echo "usage: make install CARD=/Volumes/<card>"; exit 1; }
	mkdir -p "$(CARD)/SRC/INCLUDE"
	cp cc/sd/SRC/INCLUDE/*.h "$(CARD)/SRC/INCLUDE/"
	cp -n cc/sd/SRC/*.c "$(CARD)/SRC/" || true # the examples, unless already there
	-command -v dot_clean >/dev/null && dot_clean -m "$(CARD)/SRC" # macOS ._ metadata files

install-demo:
	@test -n "$(CARD)" || { echo "usage: make install-demo CARD=/Volumes/<card>"; exit 1; }
	mkdir -p "$(CARD)/PY"
	cp upython/demo.py "$(CARD)/PY/"
	-command -v dot_clean >/dev/null && dot_clean -m "$(CARD)/PY"

clean:
	rm -rf $(OUT)
	rm -rf upython/build-embed

.PHONY: all install install-demo deploy flash-apps clean

# keep the objects for incremental rebuilds
.SECONDARY:
