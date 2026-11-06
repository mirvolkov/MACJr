# 16. On the air

The board had Wi-Fi in its silicon but not in its firmware. This step
added the station side and a program to join a network from the MK-52
keys, which needed the console's first real widget: a scrolling list.

## A window that scrolls

Networks don't fit a 40x15 screen for long, so the tty got a list window
in the DOS style, with a scroll bar of `░` and `█` at its right edge:

```
home                                *****░
office                              ***··█
cafe                                **···█
```

The app doesn't hand the list its items. The list asks for the text of
one item by index as it draws, so it can be longer than the window, and
the same code will serve file lists later. The bar started on the left and
moved right after the first look at it, which also freed the status row:
13 networks now fit at once.

## The net component

`components/hal/net` is a thin station layer: `net_wifi_scan()`,
`net_wifi_connect()`, `net_wifi_disconnect()` and `net_wifi_status()`.
Wi-Fi starts on the first call, not at boot, so programs that never touch
it pay nothing. A connection attempt waits on an event group, and the
driver's disconnect reason becomes an answer a person can read: wrong
password, network not found, no answer.

One detail took some care. Dropping a connection reports its result
later, through an event. If that event lands during the next attempt, it
looks like the new attempt failed. So every drop waits for its own event
before anything else starts.

## Bigger than the partition

Wi-Fi took the firmware from 489 KB to 1.06 MB, past the 1 MB app
partition. ESP-IDF's larger single-app layout gives it 1.5 MB. Wi-Fi and
lwIP buffers went to PSRAM where they can, and the TCP/IP task was pinned
to CPU0, leaving CPU1 to programs.

## The WIFI program

П→x and x→П move through the list, В↑ opens a password window over it,
and В↑ again connects. Passwords are digits only for now, because that is
what the keys can type. A network that connects is saved in the db as
`wifi.ssid` and `wifi.pass`, and its password is filled in next time.

The program began as a firmware built-in, which is how new apps start
now: one flash per try, no SD card round trip. Once it settled it moved to
`apps/wifi`.

## A keyboard with no seats left

After the kill work in [story 17](17-kill-and-cleanup.md), WIFI stopped
reacting to keys. `/dev/kbrd` had room for 4 readers, and the firmware
alone now held all 4 while a program ran: key beep, monitor, the kill
watch and the `sys_getkey()` queue. The program's own `open()` failed, and
it had no way to say so. The limit is 8 now, and running out logs a
warning.

**Result:** a list window any console app can use, and a program that
finds the networks around and keeps the one it joined.
