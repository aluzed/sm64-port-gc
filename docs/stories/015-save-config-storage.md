# STORY-015 — Saves and configuration on SD / memory card

**Epic:** 5 — Storage
**Status:** 🟡 Working and verified under Dolphin (Wii SD). Console settings and the GameCube
memory card remain
**Depends on:** STORY-003
**Estimate:** M (2-3 d)
**Platform:** GC + Wii

## Context

Two persistent files exist on the PC side, both ignored by `.gitignore`:

| File | Contents | Written by |
|---|---|---|
| `sm64_save_file.bin` | the N64 EEPROM (stars, times, options) | `src/pc/ultra_reimplementation.c` |
| `sm64config.txt` | resolution, fullscreen, key bindings | `src/pc/configfile.c` |

On PC these are opened with `fopen()` in the current directory. On a console there is neither
a current directory nor a mounted filesystem by default.

Two possible backing stores:

| | libfat (SD) | Memory card (`CARD_*`) |
|---|---|---|
| Wii support | internal SD slot, USB | only through a GC adapter |
| GameCube support | SD Gecko / SD2SP2 | native |
| API | standard `fopen`/`fwrite` | dedicated libogc API, 8 KB blocks |
| Complexity | low | medium (formatting, blocks, icon header) |

**Decision: libfat first.** The existing PC code then works unchanged — it only needs
`fatInitDefault()` at boot and a path prefix. The GameCube memory card is a later addition,
relevant mostly if the GameCube target is kept (STORY-005) and the user has no SD Gecko.

## What landed

`src/pc/storage_ogc.c` / `.h`. The root cause was one missing line: **`libfat` was linked from
the start but `fatInitDefault()` was never called**, so nothing was mounted and every `fopen`
in the port failed silently. The game runs fine without saving, which is exactly why it went
unnoticed for so long.

Three things were not obvious and cost a lookup each:

- **Serial Port 2 is not in libfat's automatic scan.** `fatInitDefault()` only walks the two
  memory card slots (`carda`, `cardb`). An SD2SP2 lives on `port2` and has to be mounted by
  hand with `fatMountSimple("port2", &__io_gcsd2)`, or it is never found and nothing reports
  an error. It is tried **first** on GameCube: it is the one slot a memory card cannot occupy.
- **`fatInitDefault()`'s return value is not a verdict.** It reports whether a *default* device
  was set; a manually mounted SD2SP2 is perfectly usable without one. Treating `false` as
  fatal would disable saving on exactly the setup that works.
- **The working directory cannot be relied on.** Depending on the loader a homebrew binary
  starts at the device root, at its own directory, or nowhere, so every path handed out is
  absolute.

A device is accepted only after `mkdir` **and** a write-and-delete probe: a card can be present
and mounted yet write protected, and finding that out at the first save is too late.

Saves are crash-safe. The image goes to `sm64_save_file.bin.tmp`, the old file is removed, then
the temporary is renamed over it — libfat's `rename` does not replace an existing file, so the
removal is required. That leaves a narrow window with no real file, which the read side covers
by falling back to the temporary. A power cut therefore costs the new save, never the old one.

**Verified under Dolphin** on the Wii target with the emulated SD card, read back from the
host filesystem rather than from a screenshot: `sd:/sm64/sm64config.txt` (188 bytes, real
content) and `sd:/sm64/sm64_save_file.bin` (512 bytes exactly, the EEPROM image), with no
`.tmp` left behind. This Dolphin build emulates only the Wii SD card — there is no GameCube SD
adapter to emulate — so the GameCube path is code-identical but untested off hardware.

## Goal

As a player, I want my progress and settings to survive powering the console off, so I do not
restart the game every session.

## Acceptance criteria

- [x] Progress (stars, save files A–D) persists across a power cycle. *Verified as far as
      Dolphin allows: the 512-byte EEPROM image is created and rewritten on the SD card.
      Persistence across a real power cycle still needs hardware.*
- [x] The config file is read at boot and written on exit. *Read and created at boot. Written
      on exit is **not** done — see task 5, `atexit` never fires on a console.*
- [x] With no storage present, the game **still starts** and stays playable: saving fails
      quietly, it does not crash. `storage_ogc_path` returns the bare name, so the `fopen`
      fails exactly as it did before any of this existed.
- [x] Paths are consistent and documented: `<device>:/sm64/`, absolute, device chosen by probe.
- [x] An interrupted save (power cut mid-write) does not corrupt the existing file.
- [x] The save file is compatible with the PC build's (same name, same raw 512-byte format,
      big-endian on both sides), so progress can be transferred.

## Tasks

1. **Mount the filesystem** at the very start of `main_func()`, before `configfile_load()`:
   ```c
   #ifdef TARGET_OGC
   static bool fs_ready = false;
   fs_ready = fatInitDefault();
   #endif
   ```
   `fatInitDefault()` mounts `sd:` (Wii SD slot, SD Gecko on GC) and `usb:` automatically. It
   returns `false` when there is nothing to mount — a nominal case to handle, not a fatal error.

2. **Centralise the paths.** Create `src/pc/ogc_paths.c`:
   ```c
   const char *ogc_save_path(void);    /* "sd:/apps/sm64/sm64_save_file.bin" */
   const char *ogc_config_path(void);  /* "sd:/apps/sm64/sm64config.txt"     */
   ```
   Create the directory if needed. Replace `pc_main.c`'s `#define CONFIG_FILE "sm64config.txt"`
   with a call to this.

3. **Atomic writes.** Write to `<file>.tmp`, `fflush` + `fclose`, then `rename()` onto the
   final name. A power cut mid-write then leaves the previous save intact. That is three lines
   of code and it removes the most frustrating class of bug for a player.

4. **EEPROM save.** Check how `ultra_reimplementation.c` implements `osEepromLongWrite` /
   `osEepromLongRead` in the port and hook the console path in. The format is a raw dump of the
   N64 EEPROM (512 or 2048 bytes): no conversion needed, endianness included (big-endian on
   both sides).

5. **`atexit(save_config)`.** `pc_main.c` registers the config save through `atexit`. On a
   console `main_func()` never returns (infinite loop), so `atexit` will not fire. Save
   explicitly: on HOME / RESET exit (see STORY-019), and on every setting change if an options
   menu is added.

6. **Console-specific settings** to add to `configfile.c` (its `key value` format extends
   easily):
   ```
   overscan_h        16
   overscan_v        12
   widescreen        0
   deadzone          16
   camera_invert_y   0
   ```
   Drop the meaningless PC keys (`fullscreen`, keyboard bindings) from what is written on a
   console, while still tolerating them on read so a file imported from a PC build is not
   rejected.

7. **GameCube memory card** — *no longer optional; this is now the main gap.* An SD2SP2 or an
   SD Gecko is a mod, while a memory card is what a GameCube ships with, so without this a
   stock console cannot save at all. It is a genuinely separate implementation, not a path
   change: `CARD_Init("SM64", "00")`, `CARD_Mount`, `CARD_Open`/`CARD_Create`, `CARD_Write` in
   8 KB-aligned blocks, plus a banner and icon for the memory-card manager. The 512-byte EEPROM
   image fits in one block with room to spare. Slot A first, then slot B. Keep libfat ahead of
   it in the probe order when both are present, so a save carried from the PC build still
   wins. Original:
   `CARD_Init("SM64", "00")`, `CARD_Mount`, `CARD_Open`/`CARD_Create`, `CARD_Write` in
   8 KB-aligned blocks. Provide an icon and a file comment for the memory-card manager screen.

## Files touched

- `src/pc/storage_ogc.c` / `storage_ogc.h` (new; the story called them `ogc_paths`)
- `src/pc/pc_main.c`
- `src/pc/configfile.c` / `configfile.h`
- `src/pc/ultra_reimplementation.c`
- `Makefile` (`-lfat`)

## Notes and risks

- `-lfat` must come before `-logc` in the link order, or symbols go unresolved.
- SD access is **slow and blocking**. Never write the save during an active game frame: the
  game will visibly stall. SM64 only saves at transition points (level end, file menu), which
  suits us — verify that is still the case in the port's code path.
- Dolphin emulates an SD card (`Sys/`, an `sd.raw` image): the `sd:/` path works on the
  emulator provided SD card insertion is enabled in the settings.
- Do not store saves in the Wii NAND: that would require signing and is outside simple
  homebrew.
- While here, fix the `-Wchar-subscripts` warnings in `configfile.c`: passing a signed `char`
  to `isspace()` is undefined behaviour for negative values, which `-fsigned-char` now makes
  reachable (see STORY-002).
