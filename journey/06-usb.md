# 6. USB: one stack, two personalities

Copying files onto the SD card meant pulling it out, and soon the board will
be sealed inside the MK-52 case. `esptool` only writes the ESP32's own flash.
So USBDRIVE turns the whole board into a card reader: plug the native USB port
into a computer and the card shows up in Finder.

## Sharing TinyUSB

The [microphone](02-radio.md) used TinyUSB through Espressif's UAC component,
which owned the whole USB setup. Its escape hatch,
`CONFIG_USB_DEVICE_UAC_AS_PART`, hands the configuration back to the
application. Our `usbdev` component now owns TinyUSB with both classes
compiled in, and picks which device the host sees:

```c
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  return g_usbdev_mode == USBDEV_MODE_MSC ? s_config_msc : s_config_uac;
}
```

The two use different product IDs (0x8000 and 0x8001), so the computer never
mixes up cached descriptors.

The first build failed with undefined references to callbacks we had defined.
TinyUSB is linked after our component, and nothing had asked for them yet when
our library went by. `-u tud_descriptor_device_cb` marks them as required.

## Raw sectors, one owner at a time

The host's SCSI READ(10) and WRITE(10) commands map straight onto SD card
sectors, so it sees partition table, FAT and all. Two computers writing one
FAT would corrupt it, so the mode reads the root directory for the screen,
unmounts its own FAT, reopens the card without a filesystem, and only then
starts USB. The screen shows "waiting for host", "connected", and "ejected,
safe to unplug".

Starting the drive also brought back the watchdog resets, and that led to the
[real PSRAM bug](04-boot-and-psram.md). The console survived, because every
log line also goes out of UART0 on the second USB-C port.

## Putting everything back

USBDRIVE was never meant to end. Once programs could be
[killed](09-programs.md), nothing could read the card afterwards, and USB stayed
in the mode it had started in, so USBDRIVE after a killed RADIO failed until
reboot. `usbdev_stop()` now takes USB all the way back:

1. Detach from the host, and let any transfer finish.
2. Let the TinyUSB task leave its loop, then shut the stack down.
3. Give the native port back to USB-Serial-JTAG.

Step 3 needed a closer look: `usb_del_phy()` leaves the PHY routed to USB OTG,
and what routes it back is creating a PHY for USB-Serial-JTAG. The UAC
component has no shutdown, so its mic task is put to sleep by closing the
stream, as a host would.

The order matters: USB goes down first and the card comes back last, so the
host and the ESP32 never write to it at the same time. Eject the drive before
killing USBDRIVE, though. To the host a kill is a pulled cable.

**Result:** the SD card shared with a computer at USB full speed (up to about
1 MB/s), and USB back to its default when the program ends. The teardown had
not been tried on the board when this was written.
