# 17. Copy, paste, delete

So far the navigator could only open things. Moving a file around meant
pulling the SD card and using the Mac. Now the board does it itself: the
navigator has a file menu in the style of the DOS Navigator, opened with
**F + В↑**, the MK-52's own "Alt+Enter".

```
 ╔═══════[NOTES.TXT]═══════╗
 ║ 3  View / run            ║
 ║ 8  Delete                ║
 ║ 4  Copy                  ║
 ║ 5  Paste README.TXT      ║
 ╚══════════════════════════╝
```

A digit picks the operation. The digits match their places on the keypad.
The menu only lists what makes sense for the entry. `..` has nothing to
copy or delete, a file nothing opens has no "View / run", and Paste only
appears once something has been copied.

## A chord that doesn't slow down В↑

The keyboard already had chords for the system: F + K for the monitor,
F + Cx to kill a program, and so on. A key that can start a chord is held
back until it is released or its partner arrives, because only then does
the keyboard know which one was meant. That is fine for F, which does
nothing alone, but every list in every program opens with В↑. Holding it
back would make each В↑ wait for the finger to lift.

So the new chord is *ordered*: it counts only when F is pressed first.
В↑ on its own is still sent the moment it goes down. Only F waits, as it
always did. A USB keyboard sends the same chord for Alt+Enter.

## Copy is a path, paste is the work

Copy only remembers the full path of the entry, which serves as the
clipboard. Nothing is read until Paste, so copying a big directory costs
nothing, and the same clipboard can be pasted several times.

Paste goes into the current directory, or into the selected directory
when the menu was opened on one ("Paste into X"). On `..` it goes into the
parent. A name that is already taken gets `_1`, `_2`, … before its
extension, so pasting a file next to itself makes a copy instead of an
error. A directory is copied with everything in it, but never into itself
or one of its own subdirectories, since that copy would never end.

The copy goes through a 4 KB buffer taken from the heap for the paste and
given back afterwards. The navigator lives in the firmware, and SRAM is
too tight to keep a buffer for an operation that is rarely used. A window
shows the file being copied and the bytes done every 64 KB. Cx stops the
copy, and whatever was only half made is removed. Errors, such as a full
card, come up in a window until a key is pressed.

## Deleting a tree

Delete asks first: "8 delete, Cx cancel". A directory goes with everything
in it, depth first. The directory is read again after each removal rather
than deleting entries under a `readdir` that is still running, which FAT
does not promise to survive. That makes a big directory O(n²), which does
not matter at SD card speed and a few hundred entries.

The copy and the delete share two path buffers that grow by `/name` going
down and shrink on the way back, so the recursion uses almost no stack.
That matters on the 8 KB main task.

macOS leaves a `._NAME` file next to everything it writes. The navigator
hides those, so deleting `NAME` removes its `._NAME` as well. Otherwise
the card would slowly fill with invisible orphans.
