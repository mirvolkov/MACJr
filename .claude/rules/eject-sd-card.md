---
description: Eject the MACJR SD card after copying ELFs to it
alwaysApply: true
---

After copying programs (ELFs) or any other files to the MACJR SD card
(`make install CARD=/Volumes/MACJR` in apps/, or a plain cp) and verifying
them with cmp, always eject the card: `diskutil eject /Volumes/MACJR`.
Don't wait to be asked; the card goes straight back into the board.
