# STORY-002 — `TARGET_GC` / `TARGET_WII` build targets and the devkitPPC toolchain

**Epic:** 0 — Foundations
**Status:** ✅ Done and verified — `.dol` produced for both Wii and GameCube
**Depends on:** STORY-001
**Estimate:** M (2-3 d)
**Platform:** GC + Wii

## Context

The `Makefile` knows four targets today: N64 (`TARGET_N64`), Windows (`TARGET_WINDOWS`),
Linux (`TARGET_LINUX`) and Web (`TARGET_WEB`). Target selection happens in the "Automatic
settings only for ports" block, and the toolchain in the `else # TARGET_N64` block, which
hardcodes `CC := gcc` / `CXX := g++`.

Several things are incompatible with cross-compilation:

- `CFLAGS` contains `-march=native` — invalid for PowerPC.
- `LD` is the host compiler; we need `powerpc-eabi-gcc`.
- Linking produces a native executable; we need an ELF, then `elf2dol`.
- `TARGET_WINDOWS` is inferred from `OS=Windows_NT`, which traps a cross-compilation started
  from Windows: `TARGET_WII=1` must short-circuit that detection.

## Goal

As a developer, I want `make TARGET_WII=1` (or `TARGET_GC=1`) to produce
`build/us_wii/sm64.us.dol`, so that there is a working build loop before a single line of
platform code exists.

## Acceptance criteria

- [x] `make TARGET_WII=1` and `make TARGET_GC=1` select the devkitPPC toolchain and enable no
      PC backend (`ENABLE_OPENGL`, `ENABLE_DX11`, `ENABLE_DX12` stay at 0).
- [x] Build directories are distinct — `build/$(VERSION)_wii` and `build/$(VERSION)_gc` — so
      PC and console builds coexist without `make clean`.
- [x] Host tools (`tools/`, `extract_assets.py`) still build with the **host** compiler, not
      `powerpc-eabi-gcc`.
- [x] The `.o → .elf → .dol` chain produces a non-empty `.dol`, and `elf2dol` runs
      automatically from the `all` target.
- [x] Plain `make` (PC build) behaves exactly as before: **zero regression**.
- [x] The build fails with a clear message if `$DEVKITPRO` / `$DEVKITPPC` are unset.

## Tasks

1. **Target selection block**: add `TARGET_GC ?= 0` and `TARGET_WII ?= 0`. If either is 1,
   define `TARGET_OGC := 1` and force `TARGET_WINDOWS := 0` / `TARGET_LINUX := 0` before the
   `OS` detection. Reject `TARGET_GC=1 TARGET_WII=1` with an `$(error)`.

2. **Toolchain detection**: derive paths from `$(DEVKITPRO)` / `$(DEVKITPPC)`, with an
   `$(error Set DEVKITPRO in your environment...)` when absent.

   ```make
   PREFIX  := $(DEVKITPPC)/bin/powerpc-eabi-
   CC      := $(PREFIX)gcc
   CXX     := $(PREFIX)g++
   LD      := $(PREFIX)gcc
   OBJCOPY := $(PREFIX)objcopy
   ELF2DOL := $(DEVKITPRO)/tools/bin/elf2dol
   ```

3. **Machine flags.** Follow what `gamecube_rules` / `wii_rules` do rather than inventing:

   | | GameCube | Wii |
   |---|---|---|
   | `MACHDEP` | `-mogc -mcpu=750 -meabi -mhard-float` | `-mrvl -mcpu=750 -meabi -mhard-float` |
   | `LIBOGC` | `$(DEVKITPRO)/libogc/lib/cube` | `$(DEVKITPRO)/libogc/lib/wii` |
   | Defines | `TARGET_GC`, `TARGET_OGC`, `GEKKO` | `TARGET_WII`, `TARGET_OGC`, `GEKKO` |

   `PLATFORM_CFLAGS := $(MACHDEP) -DTARGET_OGC -I$(DEVKITPRO)/libogc/include -fsigned-char`
   (see STORY-003 — `-fsigned-char` is not optional).

4. **Drop `-march=native`** from the shared `CFLAGS` and move it into the Windows/Linux
   `PLATFORM_CFLAGS` only.

5. **Linking**: `PLATFORM_LDFLAGS := $(MACHDEP) -L$(LIBOGC) -logc -lm -Wl,-Map,…`
   (`libfat`, `libwiiuse`, `libbte` come with STORY-014 and 015.)

6. **Output chain**: add `DOL := $(BUILD_DIR)/$(TARGET).dol`, point `all:` at it when
   `TARGET_OGC=1`, and add the `elf2dol` rule.

7. **Isolate host tools**: `$(MAKE) -C $(TOOLS_DIR)` must not inherit the cross compiler.

8. Add `TARGET_GC` / `TARGET_WII` to the "==== Build Options ====" banner.

## Files touched

- `Makefile` (target selection, toolchain, flags, link rules)
- `tools/Makefile` (check it does not inherit `CC`)

## Notes and risks

- **`sdl2-config` / `pkg-config`** are invoked through `$(shell ...)` under
  `ifeq ($(ENABLE_OPENGL),1)` guards. Make sure none is evaluated in a GC/Wii build, or the
  error is cryptic on a machine without SDL2.
- devkitPPC is normally used from the **devkitPro MSYS2 shell**. Document that (STORY-016):
  running `make` from `C:\devkitPro\msys2\msys2_shell.bat` avoids Windows/POSIX path issues.
- `elf2dol` can fail quietly on an ELF with unexpected sections. Check the produced file
  size, not just the exit status.
- Do **not** `include $(DEVKITPPC)/wii_rules`: those files impose their own project layout
  (`SOURCES`, `TARGET`, implicit rules) that is incompatible with sm64's `Makefile`. Take
  inspiration, do not include.

## Implementation log

### What was done

All in the `Makefile`:

- `TARGET_GC` / `TARGET_WII` declared and validated; `TARGET_OGC` derives from them and
  carries what is common. Forbidden combinations (`GC`+`Wii`, `OGC`+`N64`, `OGC`+`WEB`) raise
  an explicit `$(error)`.
- The `$(OS)` detection now sits under `ifeq ($(TARGET_WEB)$(TARGET_OGC),00)`: without that,
  a cross-compilation started from Windows silently turned itself into a native Windows build.
- devkitPPC toolchain (`powerpc-eabi-*`), per-console `MACHDEP`, libogc paths, `elf2dol`.
- `-march=native` removed from the shared `CFLAGS` and pushed down into the Windows and Linux
  `PLATFORM_CFLAGS` only.
- Separate build directories `build/<version>_wii` and `build/<version>_gc`.
- `EXE` → `.elf`, new `DOL` variable, `all:` points at `$(DOL)`, `elf2dol` rule with a check
  that the produced file is not empty.
- PC-only sources filtered out (13 files) along with every `.cpp` — details in STORY-003.
- `CPP` pointed at `powerpc-eabi-cpp` for the console target: **one less host dependency**
  (`Makefile.split` needed a system `cpp` to preprocess the level rules).

### Verification

| Check | Result |
|---|---|
| Variables (`CC`, `LD`, `BUILD_DIR`, `EXE`, `DOL`, `ELF2DOL`, flags) via `make print-%` | correct, GC and Wii |
| PC non-regression (`make print-% OS=Windows_NT`) | `-DTARGET_WINDOWS -march=native`, `-DENABLE_DX11` intact |
| `src/pc` filtering | 10 neutral files kept, 13 excluded, `CXX_FILES` empty |
| Cross-compiling the 12 port-layer files | **OK, GC and Wii, 0 errors** |
| Linking `gfx_ogc.o` + `controller_ogc.o` against libogc, then `elf2dol` | **valid `.dol`: 143,904 B (Wii), 111,744 B (GC)** |

### Full build

Once `gcc`, `python` and the base ROM were in place (see [README](README.md)):

```
make TARGET_WII=1 -j8   ->  build/us_wii/sm64.us.dol   ~12.84 MB
make TARGET_GC=1  -j8   ->  build/us_gc/sm64.us.dol    ~12.81 MB
```

All acceptance criteria are met: host tools built with the host compiler, distinct build
directories, non-empty `.dol` from the `all` target, PC build still working.

Two problems hit along the way:

1. **`gettime` unresolved at link time.** The hand-written declaration from STORY-003
   compiled, but libogc defines `gettime()` as `MK_INLINE` (`static inline`): there is no
   symbol to link against. Fixed with a real `gfx_ogc_get_ticks()` wrapper in `gfx_ogc.c` —
   the only file that can include `<ogc/lwp_watchdog.h>` without a clash — declared in
   `gfx_ogc.h`, which stays deliberately libogc-free.

2. **`msys2_shell.bat` starts in `MSYSTEM=MINGW64`**, which adds `-municode` to `armips`
   while the installed `gcc` targets Cygwin. The shell must be started in MSYS mode. Detailed
   in the [roadmap README](README.md).

Remaining warnings are all pre-existing upstream ones (`gfx_dummy.c` unused parameters,
`camera.c`, and `configfile.c` `-Wchar-subscripts`). That last one deserves a note: passing a
signed `char` to `isspace()` is undefined behaviour when the value is negative, which
`-fsigned-char` now makes possible. Harmless while the config file stays ASCII; worth fixing
with STORY-015, which already touches that file.
