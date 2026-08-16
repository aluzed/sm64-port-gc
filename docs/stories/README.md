# Roadmap — GameCube / Wii port (devkitPPC → `.dol`)

Porting `sm64-port` to PowerPC (Nintendo GameCube and Wii) with the **devkitPPC + libogc**
toolchain, producing a `sm64.dol` / `boot.dol` executable.

## Starting point

`sm64-port` is already a port: the game code (`src/game`, `src/engine`, `src/audio`, …) is
platform independent, and the whole machine layer is isolated in `src/pc/` behind three
stable interfaces:

| Interface | File | What we must write for GC/Wii |
|---|---|---|
| `GfxRenderingAPI` | `src/pc/gfx/gfx_rendering_api.h` | a **GX** backend (the big one) |
| `GfxWindowManagerAPI` | `src/pc/gfx/gfx_window_manager_api.h` | a libogc **VIDEO/VI** backend |
| `AudioAPI` | `src/pc/audio/audio_api.h` | an **AI DMA / AESND** backend |
| `ControllerAPI` | `src/pc/controller/controller_api.h` | a **PAD** (GC) / **WPAD** (Wii) backend |

So the port means adding a fourth platform alongside Windows/Linux/Web, **without touching
the game code**. That is the guiding rule for the whole roadmap.

## Two structural advantages

1. **Natively big-endian.** The PowerPC 750 (Gekko/Broadway) is big-endian, like the N64's
   R4300i. Every asset extracted from the ROM is already in the right byte order: no
   byte swapping, unlike x86 targets.
2. **The N64 colour combiner maps onto TEV.** The GX TEV pipeline is a direct descendant of
   the RDP. Translating it is far more natural than generating GLSL.

## Two major risks

1. **Memory on GameCube.** The GameCube only has 24 MB of MEM1 (plus 16 MB of ARAM, DMA
   only). The port's binary embeds every asset, so we must measure early whether it fits.
   The Wii (24 MB MEM1 + 64 MB MEM2) is comfortable.
   → **Decision: Wii first**, GameCube second (see STORY-005 and STORY-018).
2. **The GX backend.** Roughly 2,500 lines of OpenGL/D3D to rewrite in GX, including the
   combiner-to-TEV translation. That is about 60 % of the total effort.

## Milestones

| Milestone | Content | Stories |
|---|---|---|
| **M0 — It builds** | `make TARGET_WII=1` produces a `.dol` that boots to a black screen without crashing | 001 → 005 — ✅ **reached under Dolphin**, hardware pending |
| **M1 — It draws** | A textured triangle on screen through GX | 006 → 009 — ✅ **geometry on screen**, textures done |
| **M2 — The intro** | The Mario logo / title screen render correctly | 010, 011 — ✅ intro Mario head and sky render in full colour |
| **M3 — It plays** | Controller + audio: Mario is playable in the castle lobby | 012 → 014 |
| **M4 — It saves** | Persistent save file, config read at boot | 015 |
| **M5 — It ships** | `boot.dol` + `meta.xml` installable from the Homebrew Channel | 016, 017 |
| **M6 — It runs well** | Stable 30 fps, GameCube supported, polish | 018, 019 |

## Stories

### Epic 0 — Foundations
- [001 — Repo hygiene: keep ROM archives out of version control](001-repo-hygiene-gitignore.md) ✅ **done**
- [002 — `TARGET_GC` / `TARGET_WII` build targets and the devkitPPC toolchain](002-toolchain-devkitppc-makefile.md) ✅ **done**
- [003 — C portability: disable the PC backends, defuse the PowerPC traps](003-c-portability-powerpc-traps.md) ✅ **done**

### Epic 1 — Platform bring-up
- [004 — Window-manager backend: VIDEO/VI init, framebuffers, main loop](004-video-window-manager-backend.md) ✅ **done**
- [005 — MEM1 / MEM2 memory map and binary footprint](005-memory-mem1-mem2.md) 🟡 **measured, optimisation pending**

### Epic 2 — GX rendering
- [006 — GX backend skeleton (`GfxRenderingAPI`)](006-gx-backend-skeleton.md) ✅ **done**
- [007 — Translating the N64 colour combiner into TEV stages](007-colour-combiner-tev.md) ✅ **done**
- [008 — Textures: GX swizzle, cache and wrap modes](008-gx-textures.md) ✅ **done**
- [009 — Vertex format and triangle submission](009-vertex-format-draw-triangles.md) 🟡 **per-batch projection landed**; level geometry still missing, see the log for the leading hypothesis
- [010 — Effects: fog, noise, alpha compare, Z decals](010-effects-fog-noise-alpha.md)
- [011 — Video modes, resolution, PAL/NTSC and 16:9](011-video-modes-resolution.md)

### Epic 3 — Audio
- [012 — 32 kHz stereo audio backend (AI DMA)](012-audio-backend-ai-dma.md)

### Epic 4 — Input
- [013 — GameCube controller (`PAD`) → `OSContPad`](013-gamecube-controller-pad.md) 🟡 **implemented, unvalidated**
- [014 — Wii controllers (`WPAD`): Wiimote+Nunchuk, Classic, GC](014-wii-controllers-wpad.md)

### Epic 5 — Storage
- [015 — Saves and configuration on SD / memory card](015-save-config-storage.md)

### Epic 6 — Distribution
- [016 — Packaging: `.dol`, `meta.xml` and a `make dist` target](016-packaging-dol-homebrew.md)
- [017 — Test strategy: Dolphin first, then real hardware](017-testing-dolphin-hardware.md) 🟡 **loop running**

### Epic 7 — Performance and polish
- [018 — Optimisation: GX display lists, cache, GameCube support](018-performance-optimisation.md)
- [019 — Polish: crash handler, clean exit, release](019-polish-stability-release.md)

## Dependency graph

```
001 ─ 002 ─┬─ 003 ─┬─ 004 ─┬─ 005
           │       │       │
           │       │       └─ 006 ─┬─ 007 ─┐
           │       │               ├─ 008 ─┤
           │       │               └─ 009 ─┴─ 010 ─ 011
           │       │
           │       ├─ 012 (audio, parallelisable)
           │       └─ 013 ─ 014 (input, parallelisable)
           │
           └────────────────────── 015 ─ 016 ─ 017 ─ 018 ─ 019
```

The **Audio** (012) and **Input** (013-014) epics only depend on 003, so they can be worked
in parallel with the GX effort by someone else.

## Conventions

- **Do not modify `src/game`, `src/engine`, `src/audio`, `src/menu`, `levels`, `actors`.**
  If a fix seems to belong there, that is a sign an abstraction is missing in `src/pc`.
- Platform code lives in `src/pc/gfx/gfx_gx.c`, `src/pc/gfx/gfx_ogc.c`,
  `src/pc/audio/audio_ogc.c`, `src/pc/controller/controller_ogc.c`.
- Conditional compilation uses `#ifdef TARGET_GC` / `#ifdef TARGET_WII`, with `TARGET_OGC`
  defined for both (shared libogc code).
- The PC builds (Windows/Linux/Web) must keep compiling at every step: that is the
  anti-regression guard rail.

## Build environment (working ✅)

Everything lives inside devkitPro's own MSYS2 — **no second MSYS2 install needed**:

| Component | Validated version | Source |
|---|---|---|
| devkitPPC (`powerpc-eabi-gcc`) | 16.1.0 | devkitPro installer |
| Host compiler (`gcc`/`g++`) | 15.3.0, target `x86_64-pc-cygwin` | `pacman -S gcc` (`[msys]` repo) |
| Python | 3.12.13 | `pacman -S python` |
| GNU Make | 4.4.1 | preinstalled |
| Dolphin emulator | 2606a | `C:\Users\alexa\Documents\Dolphin-x64\Dolphin.exe` |

The host compiler only builds `tools/` (`n64graphics`, `mio0`, `skyconv`, `armips`, …),
which run on the PC. The game itself is compiled by devkitPPC, so MSYS2's native gcc is
fine; the upstream warning about "the package called simply `gcc`" only applies to building
the *game* for Windows.

### ⚠️ Trap: start the shell in MSYS mode, not MINGW64

`msys2_shell.bat` defaults to `MSYSTEM=MINGW64`. In that mode `uname` reports `MINGW64_NT-…`,
which makes `tools/Makefile` take its MinGW branch and pass `-municode` when linking `armips`.
The installed `gcc` targets Cygwin, not MinGW:

```
g++: error: unrecognized command-line option '-municode'; did you mean '-Wunicode'?
```

**Start the shell with `msys2_shell.bat -msys`** (or export `MSYSTEM=MSYS`): `uname` then
reports `MSYS_NT-…`, the MinGW branch is skipped, and everything builds.

Git Bash will not do: it has neither `gcc` nor `python3`, and does not mount `/opt/devkitpro`.

### Building

```sh
# from msys2_shell.bat -msys
cd /home/<user>/Documents/sm64-port
unzip -p "Super Mario 64 (USA).zip" > baserom.us.z64   # once
make TARGET_WII=1 -j8      # -> build/us_wii/sm64.us.dol
make TARGET_GC=1  -j8      # -> build/us_gc/sm64.us.dol
```

### Current state (2026-08-16)

| Target | `.dol` | Static MEM1 footprint | Dolphin |
|---|---|---|---|
| Wii | ~12.85 MB | ≈ 13.5 MB | ✅ 29.97 fps, 0 exceptions, textures rendering |
| GameCube | ~12.82 MB | ≈ 13.4 MB | ⚠️ 25.00 fps (PAL 50 Hz), 0 exceptions |

The game boots, reaches the title screen and plays its attract-mode demo. The in-game HUD
renders pixel-perfect, the intro Mario head shows its full colours and the sky renders
correctly. **Level surfaces are still smeared** by affine texture interpolation, which is now
the single blocker for a correct in-game image (STORY-009).

Three lessons from getting here:

1. **Frame pacing is a trap.** `VIDEO_WaitVSync()` paces the loop on the *retrace*, not on
   the game's frame rate. Left uncorrected, SM64 ran at 59.94 fps — double speed. Fixed by
   waiting two retraces per frame ([STORY-004](004-video-window-manager-backend.md)).
2. **The PAL 50 Hz problem is real and measured**, not theoretical: 25 fps instead of 30,
   i.e. 17 % too slow, music and physics included. That is a correctness bug, handled by
   [STORY-011](011-video-modes-resolution.md).
3. **Depth conventions do not line up.** GX wants −1 near / 0 far where `gfx_pc` gives
   0 near / 1 far ([STORY-006](006-gx-backend-skeleton.md)).

Section sizes and the GameCube verdict: [STORY-005](005-memory-mem1-mem2.md).
Dolphin test rig: [STORY-017](017-testing-dolphin-hardware.md).
