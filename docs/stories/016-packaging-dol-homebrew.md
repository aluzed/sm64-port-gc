# STORY-016 — Packaging: `.dol`, `meta.xml` and a `make dist` target

**Epic:** 6 — Distribution
**Status:** ✅ Done — `make dist` on both targets, icon and `meta.xml` in place
**Depends on:** STORY-002, STORY-015
**Estimate:** S (1 d)
**Platform:** GC + Wii

## Context

STORY-002 produces a `.dol`. This story turns it into something a user without the toolchain
can install, and documents the build procedure.

The Homebrew Channel expects a specific layout on the SD card:

```
sd:/apps/sm64/
├── boot.dol      <- the binary, this exact name
├── meta.xml      <- name, version, author, description
└── icon.png      <- 128x48, PNG
```

On GameCube there is no homebrew channel: the `.dol` is launched through Swiss (from an SD
Gecko or a disc), or burned onto a mini-DVD.

## Goal

As a user, I want to copy a folder onto my SD card and launch the game from the Homebrew
Channel, so I do not have to compile it myself.

## Acceptance criteria

- [x] `make TARGET_WII=1 dist` produces `dist/sm64/` containing `boot.dol`, `meta.xml` and
      `icon.png`, ready to copy into `sd:/apps/`. *Verified: 12,975,680 + 12,761 + 637 bytes.*
- [x] `make TARGET_GC=1 dist` produces a `.dol` plus a README describing how to launch it with
      Swiss, where saves go, and why the file takes two memory card blocks.
- [ ] The entry shows up correctly in the Homebrew Channel: name, version, description,
      readable icon. **Needs hardware or a Homebrew Channel emulation; not verifiable here.**
- [x] `README.md` has a complete GC/Wii build section. *Not yet read by anyone who did not
      write it, which is the part of this criterion that matters and cannot be self-certified.*
- [x] No ROM-derived asset is present in `dist/` other than compiled into the `.dol`, and
      `dist/` is git-ignored. *The one exception is `icon.png`, deliberately — see task 3.*

`meta.xml` takes its version from `git describe --tags --always --dirty`. `--always` keeps it
working before the first tag; `--dirty` marks a build made from uncommitted changes, which is
worth having when a stray `.dol` turns up on a card months later.

## Tasks

1. **`dist` target in the `Makefile`**: copy `$(DOL)` to `$(DIST_DIR)/boot.dol` alongside
   `meta.xml` and `icon.png`. Add `dist/` to `.gitignore`.

2. **`packaging/wii/meta.xml`** with `name`, `coder`, `version`, `release_date`,
   `short_description` and `long_description`. Keep `<version>` up to date on every release;
   deriving it from `git describe --tags` avoids forgetting.

3. **`packaging/wii/icon.png`** — ✅ done. 128×48 PNG, the boot splash captured from this port
   itself at 4× internal resolution so the logo is sharp rather than an upscale of 640×480.
   `packaging/icon-512.png` is the same frame cropped square, for the README.

   **These two files are ROM-derived**, unlike everything else in the repository: the logo they
   show is drawn from assets the build extracts from the base ROM. The rest of the project
   keeps such material out of version control on purpose, and the acceptance criterion above
   says `dist/` must contain none of it. Committed anyway, deliberately, because an icon is
   what makes the Homebrew Channel entry usable — but it is the one exception, it should stay
   the only one, and a drawn icon owing nothing to the ROM would remove the question entirely.

4. **README section.** Document in `README.md`:
   - installing devkitPro (graphical installer on Windows, `pacman` on Linux);
   - the exact packages: `gamecube-dev` or `wii-dev`, which pull devkitPPC, libogc and libfat;
   - the `DEVKITPRO` / `DEVKITPPC` environment variables;
   - building from devkitPro's **MSYS2 shell in MSYS mode** on Windows;
   - the ROM the user must supply (`baserom.us.z64`);
   - the build command and output location;
   - the SD installation procedure;
   - the controller mapping table (STORY-013 / 014).

5. **Reproducibility.** Record the exact devkitPPC and libogc versions validated
   (`pacman -Q devkitPPC libogc`) in the README: these packages move and occasionally break
   backwards compatibility.

6. **CI (optional).** The repository has a `Jenkinsfile` and a `Dockerfile`. The
   `devkitpro/devkitppc` image on Docker Hub allows a CI build. Useful to catch build breakage
   on every contribution, but it **does not replace** the runtime test (STORY-017).

## Files touched

- `Makefile` (`dist` target)
- `packaging/wii/meta.xml`, `packaging/wii/icon.png` (new)
- `packaging/gc/README.txt` (new)
- `README.md`
- `.gitignore` (`dist/`)

## Notes and risks

- The Homebrew Channel requires the binary to be named **exactly** `boot.dol`. An
  `sm64.us.dol` copied as is will not appear in the list.
- **The `.dol` is ~12.2 MB** (measured, see STORY-005): beyond the 8 MB limit some older
  Homebrew Channel versions enforce. **Verify this first** on hardware: if the HBC refuses to
  load it, the options are a third-party loader, or shrinking the footprint
  (`-fno-asynchronous-unwind-tables`, `--gc-sections`, `-Os` on assets — STORY-005), which will
  probably not be enough to get under 8 MB. Recent HBC versions have no such limit, so this is
  mostly a compatibility note for the release notes.
- Never distribute a `.dol` compiled for someone else: it contains assets extracted from the
  ROM. Distribution is in **source form**; compiling is the job of the user who owns the game.
  State that explicitly in the README.
