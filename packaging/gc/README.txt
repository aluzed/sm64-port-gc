Super Mario 64 - GameCube build
===============================

sm64.dol is a GameCube executable. It is not a disc image and it will not
boot on an unmodified console: something has to load it for you.

Launching it
------------

With Swiss, which is what most people use:

  1. Copy sm64.dol anywhere on an SD card.
  2. Boot Swiss from an SD Gecko, an SD2SP2 on Serial Port 2, or a boot disc.
  3. Browse to sm64.dol and start it.

Nothing else is needed. The game does not read anything from the SD card at
runtime, so it can sit in any folder.

Where your progress goes
------------------------

The memory card, by preference: slot 1 first, then slot 2. The save appears in
the console's memory card manager as "sm64_save_file.bin" and takes two blocks.
Two, not one, because the file is written alternately across them so that
losing power during a save costs the new one and never the one before it.

Without a memory card the game falls back to a FAT device, in this order:
Serial Port 2 (an SD2SP2), then an SD Gecko in either memory card slot. The
save then lives in /sm64/ on that card, in the same format the PC build uses,
so it can be carried between the two.

With neither, the game runs perfectly well and simply does not save.

Video
-----

The video mode follows the console. On a 50 Hz PAL setting the game still runs
at its proper 30 frames per second: the loop is paced on the clock rather than
on the retrace, so it does not suffer the 17% slowdown the original PAL release
had.

Controller
----------

A standard controller in port 1. A is jump, B grabs and punches, Z crouches,
the C stick moves the camera.

Redistribution
--------------

This binary embeds assets extracted from a Super Mario 64 ROM. Do not share
it. Share the source instead and let people build their own.
