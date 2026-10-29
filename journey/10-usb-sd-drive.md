# 10. The SD card as a USB drive

Copying files onto the SD card meant pulling it out of the board, and soon
the board will be sealed inside the MK-52 case. `esptool` cannot help: it
only writes the ESP32's own flash, and the card sits behind a separate SPI
bus. So boot menu option 4 turns the whole board into a card reader: plug
the native USB port into a computer and the card shows up in Finder.

## One USB stack, two personalities

The microphone already used TinyUSB through Espressif's UAC component, and
that component owned the whole USB setup: its own `tusb_config.h` and its
own descriptors, injected into the TinyUSB library. A drive could not
simply be added next to it.

The component has an escape hatch, `CONFIG_USB_DEVICE_UAC_AS_PART`, which
hands the configuration back to the application. Our new `usbdev`
component now owns TinyUSB with both classes compiled in, and the boot
menu decides which device the host sees:

```c
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  return g_usbdev_mode == USBDEV_MODE_MSC ? s_config_msc : s_config_uac;
}
```

The two personalities use different product IDs (0x8000 for the mic,
0x8001 for the drive), so the computer never mixes up cached descriptors.

## Raw sectors, one owner at a time

The drive works on the card's raw 512-byte sectors. The host's SCSI
READ(10) and WRITE(10) commands map straight onto SD card reads and writes:

```c
sdmmc_read_sectors(s_card, buffer, lba + offset / sector, bufsize / sector);
```

The host sees exactly what a card reader would: partition table, FAT and
all. That also means the ESP32 must stop using the card first. Two
computers writing the same FAT at once would corrupt it. So the mode reads
the root directory for the screen, unmounts its own FAT, reopens the card
without a filesystem, and only then starts USB.

## The linker drops our callbacks

The first build failed with undefined references to
`tud_descriptor_device_cb` and friends, even though we had defined them.
TinyUSB is linked after our component, and a linker only pulls an object
out of a library when something already seen needs it. Nothing had asked
for the descriptors yet when our library went by. Marking them as required
fixed it:

```cmake
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u tud_descriptor_device_cb" ...)
```

## On screen

```
 MacJr USB drive
────────────────────────────────────────
Card   USD  14.9 GB
Host   connected
Read   1.2 MB    Write  0.0 MB

Files on the card:
music                             <DIR>
```

The status changes from "waiting for host" to "connected", and to
"ejected, safe to unplug" when the computer ejects the drive.

## The old crash comes back

Starting the drive brought back the watchdog resets from story 9, right
after `USB device up`. Its console still worked, because every log line
also goes out of UART0 on the board's second USB-C port. Chasing it led
to the real cause: the LCD reset line is on GPIO35, which is one of the
octal PSRAM's data lines. PSRAM is now off until a free pin is found for
the LCD reset.

**Result:** boot option 4 shares the SD card with a computer at USB
full speed (up to about 1 MB/s). The file list on screen is the card as
it was when the mode started, and the card stays with the host until the
next reboot.
