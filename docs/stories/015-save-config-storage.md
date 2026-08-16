# STORY-015 — Saves and configuration on SD / memory card

**Epic:** 5 — Storage
**Status:** To do
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

## Goal

As a player, I want my progress and settings to survive powering the console off, so I do not
restart the game every session.

## Acceptance criteria

- [ ] Progress (stars, save files A–D) persists across a power cycle.
- [ ] The config file is read at boot and written on exit.
- [ ] With no storage present, the game **still starts** and stays playable: saving fails
      quietly (or with a single message), it does not crash.
- [ ] Paths are consistent and documented: `sd:/apps/sm64/` by default.
- [ ] An interrupted save (power cut mid-write) does not corrupt the existing file.
- [ ] The save file is compatible with the PC build's (same binary format), so progress can be
      transferred.

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

7. **GameCube memory card** — *optional sub-task, only if the GC target is confirmed*:
   `CARD_Init("SM64", "00")`, `CARD_Mount`, `CARD_Open`/`CARD_Create`, `CARD_Write` in
   8 KB-aligned blocks. Provide an icon and a file comment for the memory-card manager screen.

## Files touched

- `src/pc/ogc_paths.c` / `ogc_paths.h` (new)
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
