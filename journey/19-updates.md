# 19. Updates without the card: two firmware slots and /bin on flash

Every change used to mean two chores: flash the firmware over USB, then
pull the SD card, `make install` the programs onto it and put it back. The
board is going to be sealed inside the MK-52, so both had to become one
command. The firmware now has two slots and can update itself, and the
programs left the card for a flash partition of their own, mounted as
`/bin`.

## The flash, laid out again

The chip has 16 MB of flash, and IDF's "single app large" table used 1.5 MB
of it. The rest was empty. The new `partitions.csv`:

```
nvs        0x9000    24K
otadata    0xF000     8K    which slot boots, and its state
phy_init   0x11000    4K
ota_0      0x20000    4M    firmware
ota_1      0x420000   4M    firmware
coredump   0x820000  64K    the last crash, for later
apps       0x830000   6M    the programs: FAT, mounted as /bin
```

`nvs` stayed where it was, so the Wi-Fi calibration survived. The firmware
is 1.18 MB, so 70% of a slot is free. Changing the table took one last USB
flash of everything: bootloader, table, `otadata` and firmware. The IDF
`export.sh` is broken here, so flashing goes straight through the esptool
of IDF's Python (`python -m esptool write-flash @build/flash_args`).

## Updating the firmware, safely

`main/ota.c` writes an update into the slot that doesn't run. A power cut
at any point leaves the running firmware alone. With
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` a new firmware boots on trial and
has to confirm itself. A reset before that, from a crash, a hang or a power
cycle, sends the bootloader back to the old one. There are three ways back
from a firmware that starts but misbehaves:

- **В/О held at power-up** boots the other slot.
- **The SD card:** a `build/MACJr.bin` copied to `UPDATE/MACJR.BIN` is
  installed at boot and renamed `MACJR-<version>.BIN`, which keeps an
  archive to go back to.
- **The cable,** as before.

Before writing anything, the updater reads the app description inside the
image and checks the project name. The same chip runs another firmware, the
`esp-sdr` branch, and IDF would happily install that.

## Programs on flash

The `apps` partition is FAT with wear levelling, mounted at `/bin` by
`components/exec/appfs.c`. It is formatted on first boot ("No filesystem
detected ... Retrying after format"), and with the programs on it:

```
I (893) appfs: /bin: 6052 KB, 5540 KB free
I (1221) ota: running MACJr 0.0.4 from ota_0 (valid)
```

Programs are `/bin/tetris.elf`, in lower case as on Unix. The card's
`/BIN` is no longer searched. The navigator shows `BIN` in the card's root,
white like every directory, and hides an old `BIN` folder on the card,
whose name it would clash with. Inside, it works like any directory, so a
program can be pasted from the card into flash.

**Over the cable,** `make flash-apps PORT=...` builds an image of the whole
partition with IDF's `wl_fatfsgen.py` and writes it with esptool: 18
programs, 471 KB, in about 19 seconds. Two surprises came out of the
generator. It lowercases every long name on purpose, and it stores a name
that fits 8.3 (`cc.elf`) as a short entry only, which FAT keeps in upper
case. The navigator showed `cc.elf` next to `TETRIS.ELF`. FAT has a flag
for "show this short name in lower case" (`0x08` and `0x10` in the entry's
`NTres` byte), and FatFs on the board honours it. `flashapps.py` sets it on
the short entries before the wear-levelling sectors go on, so every name
comes out lowercase.

## Over Wi-Fi

While Wi-Fi is on (F + БП), `main/netupd.c` runs a small HTTP server:

```
GET    /info          firmware, slot, and the programs with size and CRC-32
PUT    /ota           a new firmware, streamed straight into the free slot
PUT    /bin/name.elf  a program (written as .TMP, then renamed over the old one)
DELETE /bin/name.elf
POST   /reboot
```

`make deploy HOST=<ip>` (`apps/tools/deploy.py`, standard library only)
sends the firmware if its checksum differs from the running one, waits for
the restart, then sends the programs whose size or CRC differs. The
firmware goes first because programs bind to its exports.

Wi-Fi is off at every boot, so a firmware updated over Wi-Fi would have
restarted unable to take the programs. Before such a restart the service
leaves a mark in RTC memory, which survives the reset. Seeing the mark, the
next boot rejoins Wi-Fi by itself and confirms the new firmware only once
the update service is up again. A firmware with broken networking is never
confirmed, and the next reset rolls it back. A token in the database
(`net.token`) locks the service; without one, anyone on the network can
update the board.

## Still open

- The Wi-Fi path is built but not yet tried on the board: `make deploy`,
  the restart and rejoin, and the confirmation.
- Not tried on the board either: an update from the card
  (`UPDATE/MACJR.BIN`) and В/О at power-up.
- No `macjr.local` yet: mDNS needs a component from the registry, so
  `deploy` takes the IP from the boot log (`updates: http://.../info`).
- Twice, a program was picked in the navigator a few seconds after boot
  with no key pressed. It looks like a stray key at start-up.
- The card still holds an old `/BIN` (hidden in the navigator) waiting to
  be deleted.
