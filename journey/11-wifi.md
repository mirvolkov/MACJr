# 11. On the air

The board had Wi-Fi in its silicon but not in its firmware. This step added the
station side and a program to join a network from the MK-52 keys, which needed
the console's [list window](03-display-and-tty.md).

## The net component

`components/hal/net` is a thin station layer: `net_wifi_scan()`,
`net_wifi_connect()`, `net_wifi_disconnect()` and `net_wifi_status()`. Wi-Fi
starts on the first call, so programs that never touch it pay nothing. The
driver's disconnect reason becomes an answer a person can read: wrong password,
network not found, no answer.

Dropping a connection reports its result later, through an event. If that lands
during the next attempt, it looks like the new attempt failed. So every drop
waits for its own event before anything else starts.

## Bigger than the partition

Wi-Fi took the firmware from 489 KB to 1.06 MB, past the 1 MB app partition.
ESP-IDF's larger single-app layout gives it 1.5 MB. Wi-Fi and lwIP buffers went
to PSRAM where they can, and the TCP/IP task was pinned to CPU0, leaving CPU1 to
programs.

## The WIFI program

П→x and x→П move through the list, В↑ opens a password window, and В↑ again
connects. Passwords are digits only for now, because that is what the keys can
type. A network that connects is saved in the db as `wifi.ssid` and `wifi.pass`,
and its password is filled in next time.

**Result:** a program that finds the networks around and keeps the one it
joined.
