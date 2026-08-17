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
| **M0 — It builds** | `make TARGET_WII=1` produces a `.dol` that boots to a black screen without crashing | 001 → 005 — ✅ **reached on real hardware** |
| **M1 — It draws** | A textured triangle on screen through GX | 006 → 009 — ✅ **geometry on screen**, textures done |
| **M2 — The intro** | The Mario logo / title screen render correctly | 010, 011 — ✅ **the game renders**: levels, models, HUD, correct perspective |
| **M3 — It plays** | Controller + audio: Mario is playable in the castle lobby | 012, 013 — ✅ **on a real GameCube**; 014 (Wii remotes) written, awaiting a Wii |
| **M4 — It saves** | Persistent save file, config read at boot | 015 — ✅ **reached**: SD and GameCube memory card, crash-safe |
| **M5 — It ships** | `boot.dol` + `meta.xml` installable from the Homebrew Channel | 016 — ✅ packaged; 017 — ✅ launched from Swiss on hardware |
| **M6 — It runs well** | Stable 30 fps, GameCube supported, polish | 018, 019 — the game is **playable on a real GameCube**: castle, first level, audio |

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
- [009 — Vertex format and triangle submission](009-vertex-format-draw-triangles.md) ✅ **done** — hardware perspective on, near-plane clipping handled by the GP
- [010 — Effects: fog, noise, alpha compare, Z decals](010-effects-fog-noise-alpha.md) 🟡 all four implemented; **validation on screen pending**
- [011 — Video modes, resolution, PAL/NTSC and 16:9](011-video-modes-resolution.md) 🟡 cadence, mode selection, 480p and 16:9 done; overscan remains

### Epic 3 — Audio
- [012 — 32 kHz stereo audio backend (AI DMA)](012-audio-backend-ai-dma.md) ✅ **done**

### Epic 4 — Input
- [013 — GameCube controller (`PAD`) → `OSContPad`](013-gamecube-controller-pad.md) ✅ **done** — validated on a real GameCube
- [014 — Wii controllers (`WPAD`): Wiimote+Nunchuk, Classic, GC](014-wii-controllers-wpad.md) 🟡 **implemented**, not yet run on a Wii — Classic and Nunchuk mapped, GameCube pad still wins port 1

### Epic 5 — Storage
- [015 — Saves and configuration on SD / memory card](015-save-config-storage.md) ✅ **done** — SD and GameCube memory card, both crash-safe and verified

### Epic 6 — Distribution
- [016 — Packaging: `.dol`, `meta.xml` and a `make dist` target](016-packaging-dol-homebrew.md) ✅ **done** — `make dist` on both targets
- [017 — Test strategy: Dolphin first, then real hardware](017-testing-dolphin-hardware.md) 🟢 **it runs on a real GameCube**; formal protocol pending

### Epic 7 — Performance and polish
- [018 — Optimisation: GX display lists, cache, GameCube support](018-performance-optimisation.md)
- [019 — Polish: crash handler, clean exit, release](019-polish-stability-release.md)

### Open defects
- [020 — Shadows flicker and drop out](020-shadow-decal-flicker.md) ✅ **fixed and confirmed on hardware**
- [021 — Textures drop in and out with camera movement](021-texture-dropouts.md) 🟡 hugely reduced by the STORY-009 fit work; one or two cases remain, footage captured
- [022 — Alignment exception entering the castle](022-alignment-traps-audit.md) ✅ **done** — fixed and confirmed on hardware, audit run, `-DCHECK_POOL_ALIGNMENT` guard in place

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

The game boots, reaches the title screen and plays its attract-mode demo. Level geometry,
character models, textures and the HUD all draw, in correct perspective.

**The image is close but not yet correct**, and earlier notes in this repo overstated it. The
intro Mario head had dark jagged polygons across the face; that is fixed (see below), and it
now shows eyes with irises and pupils, eyebrows, moustache and the cap logo. Remaining known
defects: the eye highlights are too large, and the brown parts render too dark.

What has been established by measurement rather than argument:

| Checked | Verdict |
|---|---|
| Texture upload and swizzle | ✅ correct — Dolphin's texture dump shows crisp textures with working alpha |
| Combiner input routing | ✅ correct — at most one input varies per batch (`-DGFX_GX_DEBUG_INPUTS`) |
| Texture coordinates | ✅ correct (`-DGFX_GX_DEBUG_UV`) |
| Combiner alpha output | ✅ correct — opaque geometry at 1.0, sparkle cutouts show their shape (`-DGFX_GX_DEBUG_ALPHA`) |
| Combiner translation case | ✅ no surface uses the two-stage general form or `SHADER_TEXEL0A` (`-DGFX_GX_DEBUG_CC`) |
| Per-batch projection fit | ✅ correct — thresholds relative, coefficients derived from the CPU path |
| Depth axis and sort order | ✅ correct — `z/w - 1` with `GX_LEQUAL` |

The jagged face was the perspective fit from STORY-009. Its two thresholds were absolute where
they had to be relative: a batch qualified for the hardware path on a spread in `1/w` of
`1e-9`, and the fit was accepted on an absolute residual of `1e-3`. The intro head spans about
2 % of the depth range, so a fit 5 % wrong across the object passed validation and scrambled
which of its own polygons was in front. Both thresholds are now relative to the batch's own
spread.

**Audio works** (STORY-012): measured at 32 kHz stereo out of Dolphin's DSP dump, with a
lag-1 autocorrelation of 0.965 — a real waveform, not noise.

A frame flicker that had been present since the video backend landed is also fixed: the
EFB→XFB copy sat in `swap_buffers_begin`, which runs once per *display list* rather than once
per frame, so each copy cleared the EFB and wiped the previous one's work. Two frames in three
showed the background with no Mario head. See [STORY-004](004-video-window-manager-backend.md)
— including the method lesson, since single screenshots cannot show a flicker and several
earlier conclusions in this repo were distorted by it.

The depth path is now correct as well, after three faults that had stacked on top of each
other: the mapping (`z/w - 1`, not a negation — it is three lines of `gfx_pc.c`, see
[STORY-006](006-gx-backend-skeleton.md)), two conventions writing the same buffer because the
per-batch projection derived depth independently of the CPU path
([STORY-009](009-vertex-format-draw-triangles.md)), and the EFB→XFB copy moving GX state
behind `gfx_gx.c`'s state cache ([STORY-010](010-effects-fog-noise-alpha.md)). The last one
made the 3D scene and the HUD take turns, frame by frame.

Still missing: the rest of the combiner effects — fog, noise, Z decal validation (STORY-010) —
and mode selection, 16:9 and overscan (STORY-011). One known rendering defect remains: the
water surface flickers out for an instant during the attract demo.

Lessons from getting here:

1. **Frame pacing is a trap.** `VIDEO_WaitVSync()` paces the loop on the *retrace*, not on
   the game's frame rate. Left uncorrected, SM64 ran at 59.94 fps — double speed. Fixed by
   waiting two retraces per frame ([STORY-004](004-video-window-manager-backend.md)).
2. **The PAL 50 Hz problem is real and measured**, not theoretical: 25 fps instead of 30,
   i.e. 17 % too slow, music and physics included. That is a correctness bug, fixed by pacing
   50 Hz modes on the clock ([STORY-011](011-video-modes-resolution.md)).
3. **Depth conventions do not line up.** GX wants −1 near / 0 far where `gfx_pc` gives
   0 near / 1 far ([STORY-006](006-gx-backend-skeleton.md)).
4. **Read conventions in the source; measure only behaviour.** The depth axis was reversed for
   two sessions on the strength of a screenshot, when the answer was three lines of
   `gfx_pc.c`. A measurement is evidence only once the thing being measured is known to be
   sound — and that buffer was being corrupted by two other faults at the time.
5. **When something used to work, bisect before hypothesising.** The good state was in the
   history the whole time; correlating a timestamped screenshot against `git log` found it in
   one command, after five builds spent guessing forward.
6. **The GX state cache cannot see writes made outside `gfx_gx.c`.** Anything that touches GX
   state elsewhere — today only the EFB→XFB copy — has to be re-emitted at the top of the
   next frame.

Section sizes and the GameCube verdict: [STORY-005](005-memory-mem1-mem2.md).
Dolphin test rig: [STORY-017](017-testing-dolphin-hardware.md).
