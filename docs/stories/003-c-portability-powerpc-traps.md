# STORY-003 — C portability: disable the PC backends, defuse the PowerPC traps

**Epic:** 0 — Foundations
**Status:** ✅ Done and verified — the whole game compiles and links for PowerPC
**Depends on:** STORY-002
**Estimate:** M (2-3 d)
**Platform:** GC + Wii

## Context

With the toolchain in place, compilation fails en masse: `src/pc/` holds SDL2, OpenGL,
Direct3D, XInput, ALSA, PulseAudio, WASAPI and libusb files that are selected by `#ifdef`
but still **compiled**, because the `Makefile` globs `src/pc/gfx`, `src/pc/audio` and
`src/pc/controller`.

Most of them are wrapped in a top-level `#ifdef` (`gfx_opengl.c` starts with
`#ifdef ENABLE_OPENGL`) so they compile to nothing. The rest have to be excluded explicitly.

This is also where PowerPC ABI differences get settled, including a **critical** one:

> On PowerPC, `char` is **unsigned** by default. On x86 it is signed.

SM64 constantly relies on signed `s8` / `char` semantics (angles, camera indices, counters
decremented past zero). Without `-fsigned-char` the game compiles perfectly and then
misbehaves arbitrarily. It is the most expensive bug in the whole port to diagnose — so we
defuse it before writing any code.

## Goal

As a developer, I want the entire game plus the `src/pc` layer to compile and link for
PowerPC with no leftover PC dependency, so that milestone M0 (a produced `.dol`) is reached.

## Acceptance criteria

- [x] `make TARGET_WII=1 -j8` completes without a new error or warning, through to the `.dol`.
- [x] The generated `.map` contains **no** SDL, GL, D3D, XInput, ALSA, Pulse, WASAPI or libusb
      symbol.
- [x] `-fsigned-char` is in the target `CFLAGS`, and a static assertion checks it:
      `_Static_assert((char)-1 < 0, "char must be signed");`
- [x] No change outside `src/pc/`, `Makefile` and `include/`.
- [x] The Windows and Linux builds still compile.

## Tasks

1. **Inventory the `src/pc` files to exclude.** Filter `C_FILES` / `CXX_FILES` in the
   `Makefile` under `ifeq ($(TARGET_OGC),1)`:
   - `src/pc/gfx/`: drop `gfx_opengl.c`, `gfx_sdl2.c`, `gfx_glx.c`, `gfx_direct3d*.cpp`,
     `gfx_dxgi.cpp`, `gfx_direct3d_common.cpp` — keep `gfx_pc.c`, `gfx_cc.c`, `gfx_dummy.c`
     and the future `gfx_gx.c` / `gfx_ogc.c`.
   - `src/pc/audio/`: drop `audio_alsa.c`, `audio_pulse.c`, `audio_sdl.c`, `audio_wasapi.cpp`
     — keep `audio_null.c` and the future `audio_ogc.c`.
   - `src/pc/controller/`: drop `controller_sdl.c`, `controller_xinput.c`,
     `controller_wup.c`, `wup.c`, `controller_emscripten_keyboard.c`.
   - Drop **every** `.cpp`: no C++ is needed any more, which lets us keep `LD := $(CC)` and
     avoid linking `libstdc++`.
   Filtering files rather than adding `#ifdef`s keeps upstream sources untouched and future
   rebases on `sm64-port` cheap.

2. **`-fsigned-char`** in `PLATFORM_CFLAGS`, with the static assertion in `src/pc/compat.h`.

3. **`src/pc/pc_main.c`**: add the backend selection branch and neutralise the PC-only calls
   (`set_keyboard_callbacks`, `set_fullscreen_changed_callback`).

4. **`src/pc/controller/controller_entry_point.c`**: register the console backend in place of
   the PC ones.

5. **Missing headers.** libogc ships a partial newlib libc. Check `configfile.c` (uses
   `assert.h`, `ctype.h`, `stdio.h` — all present) and `dlmalloc.c` (to be excluded:
   `USE_SYSTEM_MALLOC` is already on for ports and newlib provides `malloc` on the libogc
   arena).

6. **`src/pc/ultra_reimplementation.c`**: `osGetTime()` returns a hardcoded 0. Wire it to
   libogc's time base.

7. **Sweep for ABI traps** (a review exercise, not a blind one):
   - `long` is 4 bytes on 32-bit PowerPC as on 32-bit x86/Windows → fine.
   - Pointers are 4 bytes; `uintptr_t` is already used in `osPiStartDma` → fine.
   - Alignment: the Gekko raises alignment exceptions on some unaligned float accesses. N64
     assets are naturally aligned, but buffers allocated for GX must be 32-byte aligned
     (STORY-006/008).
   - Endianness: big-endian on both sides → **no** byte swapping to write. That is the gift
     of a PowerPC port; do not add conversions "just in case".

## Files touched

- `Makefile` (`C_FILES` / `CXX_FILES` filters)
- `src/pc/pc_main.c`
- `src/pc/compat.h`
- `src/pc/controller/controller_entry_point.c`
- `src/pc/ultra_reimplementation.c`

## Notes and risks

- `gfx_dummy.c` is an excellent stepping stone: keeping it lets us reach M0 (the game runs,
  draws nothing) and validate boot, audio and controller separately before tackling GX.
  **Recommendation: first `.dol` with `ENABLE_GFX_DUMMY`.**
- `printf` is `#define`d to nothing in `pc_main.c`. Console debugging (USB Gecko, `CON_Init`)
  will need a separate macro that this redefinition does not affect.
- Take the new `-Wall -Wextra` warnings seriously on PowerPC (implicit promotions,
  signed/unsigned comparisons): they often flag real porting bugs.

## Implementation log

### The important discovery: `gccore.h` ✗ `PR/gbi.h`

`<gccore.h>` pulls in `<ogc/gx.h>` and `<ogc/gu.h>`, which declare `Vtx` and `Mtx`.
`ultra64.h` pulls in `PR/gbi.h`, which declares **the same names** with incompatible types.
The two cannot coexist in one translation unit:

```
include/PR/gbi.h:1142: error: conflicting types for 'Vtx'
include/PR/gbi.h:1195: error: conflicting types for 'Mtx'
```

(Plus two harmless redefinitions, `_SHIFTL` and `_SHIFTR`.)

This constraint shapes the whole port layer. Rule adopted:

> **Never include `<gccore.h>` in a translation unit that includes `ultra64.h`.**
> Include the specific libogc header you need instead.

In practice:

| File | Includes | Why |
|---|---|---|
| `gfx_ogc.c` | `<gccore.h>` + `<ogc/lwp_watchdog.h>` | includes no sm64 header |
| `controller_ogc.c` | `<ogc/pad.h>` only | also needs `ultra64.h` for `OSContPad` |
| `ultra_reimplementation.c` | goes through `gfx_ogc.h` | `<ogc/lwp_watchdog.h>` reaches `ogcsys.h` → `gx.h` |

Verified: `<ogc/pad.h>` and `ultra64.h` compile together without conflict. libogc's `u8`…`u64`
typedefs are identical to `PR/ultratypes.h`'s, so they are tolerated.

Consequence for STORY-006: `gfx_gx.c` cannot include `PR/gbi.h`. Not a problem — the backend
only uses `G_TX_MIRROR` (0x1) and `G_TX_CLAMP` (0x2), which are trivially defined locally.

### What was done

- **Filtered** 13 PC-only files and every `.cpp` (in the `Makefile`, see STORY-002). Ten
  neutral files remain in `src/pc`. `dlmalloc.c` is excluded: without `USE_DL_PREFIX` it
  *replaces* `malloc`, which would fight libogc/newlib over the system arena.
- **`-fsigned-char`** in `PLATFORM_CFLAGS`, with the guard rail in `src/pc/compat.h`:
  `_Static_assert((char) -1 < 0, ...)`.
- **`pc_main.c`**: backend selection branch for `TARGET_OGC`, `set_keyboard_callbacks`
  neutralised, `controller_keyboard.h` guarded.
- **`controller_entry_point.c`**: `TARGET_OGC` branch registering `controller_ogc`.
- **`ultra_reimplementation.c`**: `osGetTime()` now returns the time base register instead
  of `0`.
- **`gfx_dummy.c`**: the dummy *window manager* is guarded out under `#ifndef TARGET_OGC` —
  its frame limiter uses `clock_nanosleep`, which newlib does not provide. The dummy
  *renderer* is kept: it is what lets the port boot before the GX backend exists.

### Verification

All 12 port-layer files compile for **Wii and GameCube**, 0 errors. Warnings: only the ones
already present upstream (`unused variable`, `maybe-uninitialized` in `gfx_pc.c`) — **no
PowerPC portability warning**, which is the signal we were looking for.

GC/Wii difference hit: `SYS_SetPowerCallback` and `SYS_ResetSystem(SYS_POWEROFF, …)` only
exist on Wii (the GameCube has no soft power button). Guarded with `#ifdef TARGET_WII` in
`gfx_ogc.c`.

### Final check on the full build

The entire game (~400 translation units) compiles and links for Wii **and** GameCube.

"No leftover PC symbol" is met. Searching `build/us_wii/sm64.us.map` for
`SDL|opengl|glew|d3d1[12]|dxgi|xinput|alsa|pulse|wasapi|libusb` returns only three substring
false positives — `sTrian`**`gleW`**`ave` (×2) and **`sDL`**`GenTime`. The only `src/pc`
objects linked are the 12 expected neutral files.

One last problem surfaced at link time: `gettime` unresolved. The hand-written declaration
compiled, but libogc defines `gettime()` as a `static inline` — there is no symbol at all.
Fixed with a `gfx_ogc_get_ticks()` wrapper defined in `gfx_ogc.c` and declared in
`gfx_ogc.h` (which includes no libogc header and therefore stays usable from a unit that
includes `ultra64.h`). The "never include `<gccore.h>` next to `ultra64.h`" rule therefore
has a corollary:

> **Any libogc service needed by an "sm64 side" unit must go through a non-inline function
> exposed by a "libogc side" unit.**
