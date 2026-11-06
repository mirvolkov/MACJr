# 4. The boot menu

With a TTY demo, a radio and USB audio all wanting the same board, we
needed a way to choose at power-up. So we made a `boot` component that
draws a start screen straight onto the display and waits for a key:

```
          MacJr 0.0.2
 ───────────────────────────────────────
  1 - Load TTY demo
  2 - Init SDR with UAC output

  Select mode: press 1 or 2
```

The title wanted a bold 24 px font, which we did not have. Instead we
scale the 8x16 font by 3/2 to 12x24 on the fly, nearest-neighbour style.
The version comes from the build (`PROJECT_VER` in CMake), so the screen
always shows the firmware that is actually running.

## Bug one: a doubled title

The first screen showed the title twice and digits in odd places. The
display driver collects drawing in a 40-row buffer and sends it in one go,
but it always sent full 320-pixel rows. The TTY redraws every row
completely, so it never noticed. The menu draws only part of each row,
and the rest of the row went out with stale pixels from an earlier batch,
including the title.

We now track which pixels in each row were really drawn and send only
those. Fully drawn rows still go out together, so the TTY is as fast as
before.

## Bug two: green came out magenta

Next, the green title showed up purple. Each pixel is two bytes: the panel
reads the high byte first, the ESP32 stores the low byte first, and nothing
swapped them. Green `0x57EA` reached the panel as `0xEA57`, which is
purple. An old "BGR" setting had hidden this for pure red only. One swap
fixed every colour:

```c
uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
return (uint16_t)((c << 8) | (c >> 8));
```

Both fixes were in the display driver, so every future screen benefits.

## Later: drawing while sending

Much later, a program that redraws the whole text area every frame showed
the screen being painted from top to bottom. The driver drew a 40-row
strip into its buffer, sent it, and *waited* for the transfer before
drawing the next strip. So the CPU and the SPI bus took turns, and a full
frame took both times added up.

Now there are two strip buffers. `lcd_blit_async()` only queues a strip
and returns a ticket, and drawing goes on in the other buffer. A buffer is
reused only after `lcd_wait(ticket)` says its last transfer is done.
Strips finish in the order they were queued, so a counter in the
"transfer done" interrupt is all the bookkeeping needed. A frame now
takes about as long as the longer of the two jobs, not the sum.

Some sweep is left, because the panel shows pixels as they arrive. Only
syncing to the panel's own refresh (its TE pin) would hide it completely.

**Result:** a menu on every start, with a green title and white options,
and keys that now use the MK-52 names (`KEY_MK_1`, `KEY_MK_2`).

*Later:* the menu takes ten programs, the tenth on key 0
([story 18](18-screenshots-and-glyphs.md)).
