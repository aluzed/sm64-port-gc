# STORY-017 — Test strategy: Dolphin first, then real hardware

**Epic:** 6 — Distribution
**Status:** 🟢 **It runs on a real GameCube** (2026-08-16). Dolphin loop running; the formal
hardware protocol below still to be worked through
**Depends on:** STORY-002 *(set this up with the very first `.dol`)*
**Estimate:** S (1-2 d to set up, then continuous)
**Platform:** GC + Wii

## First hardware run — 2026-08-16

**A real GameCube, booted through Swiss from an SD2SP2 on Serial Port 2.**

| | Result |
|---|---|
| Boots | ✅ |
| Audio | ✅ no crackle reported |
| Picture | ✅ |
| Frame rate | ✅ smooth |

This is the result the whole Dolphin loop existed to reach, and it clears the risks Dolphin
structurally cannot report: a missing `DCFlushRange` leaves the GP or the DSP reading stale
cache lines, a misaligned buffer breaks DMA, and an underfed audio DMA crackles. **None of
them showed.** Every `DCFlushRange` and every 32-byte alignment in the GX and AI backends was
written blind against the hardware manual; they were right.

What did not survive contact, and now has a ticket each:

- [STORY-020](020-shadow-decal-flicker.md) — tree shadows shimmer and drop out. Reported here
  first: the Z-decal bias is missing, and z-fighting on a large flat ground is far more visible
  on a CRT than in a scaled-up emulator window.
- [STORY-021](021-texture-dropouts.md) — textures drop in and out with camera movement. Already
  known under Dolphin; hardware confirms it is not an emulator artefact.

### Second run: a crash Dolphin structurally could not have found

Opening the castle door crashed with `Exception (Alignment)`, PC `0x80005B64`.

Resolved with `addr2line` against the build's ELF, then confirmed by disassembling the
faulting instruction rather than inferring it:

```
80005b5c:  stw   r3,4(r3)      <- unaligned, fixed up in hardware, passes
80005b60:  stw   r3,8(r3)      <- same
80005b64:  stfs  f1,20(r3)     <- r3 = 0x80FE73AA, so EA = 0x80FE73BE: traps
```

`init_graph_node_ortho_projection`, reached through `level_cmd_begin_area`. The pointer it was
handed was already two bytes out.

Cause, in `alloc_only_pool_alloc`: the `USE_SYSTEM_MALLOC` variant advanced its bump pointer by
the raw requested size, never rounding it, while the variant right below it has always used
`ALIGN4`. `level_cmd_set_terrain_data` allocates
`get_area_terrain_size() * sizeof(Collision)` bytes and `Collision` is an `s16`, so an odd
number of collision entries leaves the pool two bytes off for everything allocated after it.
The castle interior has such an area; the courtyard does not, which is why it appeared exactly
at that door.

**Dolphin cannot find this.** Its JIT performs unaligned accesses instead of raising the
alignment exception, so the float store simply succeeds there. No amount of emulator testing
would have surfaced it — which is the entire argument of this story, demonstrated.

Worth noting what did *not* need debugging: the two integer stores immediately before the
faulting one are equally unaligned and went through untouched. Reading the disassembly rather
than guessing from the symbol name is what made that obvious.

### Third run: fixed

With the allocator aligned, the castle loads and the first level is playable, audio included.
A real GameCube now runs this port past the point where it used to die.

### Fourth run: the rendering pass lands

Shadow bias, corrected display aspect, 16:9 and 480p detection, all in one build.

**"Enormous reduction in flickering almost everywhere."** Shadows stable. One or two textures
still misbehave, captured on video for later, but nothing resembling what it was.

Two of those four could only ever have been judged here: the shadow bias, whose failure modes
are opposite — too little and it shimmers, too much and shadows lift off slopes — and the
display aspect, which on a PAL set had the HUD inset by fifteen units a side.

Still unverified on hardware: a long session for audio drift, the memory card save surviving a
power cycle, 50/60 Hz switching, and overscan on a CRT.

## Context

This story does not come at the end of the project: it has to be working from milestone M0.
Every story in the GX epic is validated visually, and without a fast test loop the port becomes
a series of blind compilations.

**Dolphin is installed on this machine:** `C:\Users\alexa\Documents\Dolphin-x64\Dolphin.exe`
(with `DolphinTool.exe` for command-line work).

The essential point to internalise from the start: **Dolphin is more forgiving than hardware.**
In particular it hides three classes of bug that will show up on a console:

| Bug hidden by Dolphin | Symptom on hardware |
|---|---|
| Missing `DCFlushRange` before a GP/DSP read | corrupted textures, choppy sound, intermittent |
| Buffers not 32-byte aligned | alignment exception, exception code screen |
| Audio DMA starvation | crackling, silences |

Corollary: **schedule a hardware test at every milestone**, not just at the end.

## Goal

As a developer, I want the shortest possible compile → run → observe loop, plus a hardware
validation protocol, so regressions are caught quickly and hardware-specific bugs are found
before they pile up.

## Acceptance criteria

- [x] A `make run` target launches the freshly built `.dol` in Dolphin.
- [x] The game's output is observable while it runs (FPS counter and log file).
- [x] A written manual test protocol covers the reference scenes (list below).
- [ ] Every milestone M0→M6 has been validated at least once on **real hardware**, with the
      result recorded in this document.
- [ ] Reference screenshots (PC build) exist for the STORY-007 visual comparisons.

## Tasks

1. **`run` target** in the `Makefile`:
   ```make
   DOLPHIN ?= dolphin-emu
   run: $(DOL)
   	"$(DOLPHIN)" -b -e "$(abspath $(DOL))"
   ```
   (`-b` = batch mode: quits when the game closes, skipping the game list.) Keep `DOLPHIN`
   overridable so the path is not hardcoded for other contributors.

2. **Debug output.** Two complementary routes:
   - **Dolphin**: enable `Options → Configure → Interface → Show Debugging UI`, then
     `View → Log`. The log is also written to `User/Logs/dolphin.log`. Enable the `OSREPORT`
     type to catch the game's `printf`s.
   - **On console**: `CON_Init` writing to the framebuffer (usable before GX takes over), or a
     USB Gecko via `gecko_printf` if an adapter is available.

   Careful: `pc_main.c` contains `#define printf`, which neutralises every `printf`. Provide a
   separate `OGC_LOG(...)` macro that the redefinition does not affect.

3. **Useful Dolphin settings for this port:**
   - `Config → General → Enable Panic Handlers`: **on** during interactive use — they report GX
     errors hardware would swallow silently. **Off** for automated runs (see the rig below).
   - `Config → Wii → Insert SD Card`: needed to test STORY-015.
   - Also test with the **software renderer** (`Graphics → Backend`): very slow, but it is the
     most faithful GX rendering, useful to settle a doubt about a TEV configuration.

4. **Reference scenes** — the manual protocol, to replay at each milestone:

   | # | Scene | What it validates |
   |---|---|---|
   | 1 | Boot screen / Mario logo | Goddard rendering, textures, simple combiner |
   | 2 | Title screen and file select | 2D UI, text, transparency |
   | 3 | Castle lobby | vertex lighting, multiple textures, camera |
   | 4 | Bob-omb Battlefield (from the summit) | fog, draw distance, throughput |
   | 5 | Cool Cool Mountain (slide) | frame rate, transparency, snow |
   | 6 | Jolly Roger Bay (underwater) | water tint, complex combiner, alpha |
   | 7 | Painting room | texture edge, decals (shadows) |
   | 8 | Koopa race | precise controls, stable cadence |

5. **Reference screenshots.** Run the PC build on the 8 scenes and store the captures in
   `docs/reference/`. Side-by-side comparison is the only reliable way to validate STORY-007.

6. **Validation log.** Keep in this document:

   | Milestone | Date | Machine | Result | Notes |
   |---|---|---|---|---|
   | M0 (build) | 2026-08-16 | — | ✅ | `.dol` produced: Wii 12,838,432 B, GC 12,810,432 B |
   | M0 (Dolphin, Wii) | 2026-08-16 | Dolphin 2606a, D3D11 | ✅ | 30 s stable, **29.97 fps**, 0 exceptions |
   | M0 (Dolphin, GC) | 2026-08-16 | Dolphin 2606a, D3D11 | ⚠️ | 30 s stable, 0 exceptions, but **25.00 fps** (PAL 50 Hz — see STORY-011) |
   | M0 (hardware) | | Wii / GC | ⏳ | also check the HBC accepts a 12 MB `.dol` |
   | M1 (Dolphin, Wii) | 2026-08-16 | Dolphin 2606a, D3D11 | ✅ | test triangle correct; game geometry rasterised (verified with false colours) |
   | M1 (hardware) | | Wii / GC | ⏳ | this is where missing `DCFlushRange` calls will show |
   | M2 (Dolphin, Wii) | 2026-08-16 | Dolphin 2606a, D3D11 | ✅ | after STORY-007: intro Mario head in full colour, sky correct, HUD pixel-perfect, 0 exceptions |
   | M2 (Dolphin, Wii) — full scene | 2026-08-16 | Dolphin 2606a, D3D11 | ✅ | after the depth-comparison fix: Bowser in the Dark World renders with model, tiled floor in correct perspective, coins, Mario's shadow, HUD; 0 exceptions |
   | M2 (hardware) | | Wii / GC | ⏳ | |
   | M3 | | | | |

7. **PC non-regression.** On every port commit, check that plain `make` (the default Windows
   build) still compiles. That is the guard rail for the "do not touch the game code" rule.

## The test rig in use

Dolphin was switched to **portable mode** (`portable.txt` next to `Dolphin.exe`), which puts
its configuration and logs under `Dolphin-x64/User/` — isolated, inspectable, and removable in
one go. Three files are pre-written there so a run is never blocked by a modal window:

- `User/Config/Dolphin.ini`: `Analytics.PermissionAsked = True` (otherwise a dialog opens on
  first launch), `Interface.ConfirmStop = False`, and importantly
  **`Interface.UsePanicHandlers = False`** — Dolphin panics are modal dialogs that would freeze
  an automated test. The messages still go to the log.
- `User/Config/Logger.ini`: `WriteToFile = True`, `Verbosity = 5`, `OSREPORT`, `VIDEO`, `BOOT`
  and `DSPHLE` logs enabled → `User/Logs/dolphin.log`.
- `User/Config/GFX.ini`: `ShowFPS = True`.

**`ShowFPS` is the only usable sign of life while the renderer is incomplete**: the screen is
black, so a screenshot cannot tell "it is running" from "it crashed". The frame counter settles
it immediately — and it is what revealed the STORY-004 cadence bug.

Automatable protocol: launch `Dolphin.exe -b -e <dol>`, wait ~30 s, capture the screen, read the
counter, then `grep -E "E\[|Exception|panic" User/Logs/dolphin.log`.

### Reaching real 3D without driving the controller

Injecting keystrokes with `SendKeys` **does not work**: Dolphin's "DInput/0/Keyboard Mouse"
backend reads raw device state and ignores synthetic key events.

There is no need to fight it: **letting the game idle ~40 s on the title screen is enough** —
SM64 then starts an attract-mode demo in a real level by itself. That is how the in-game HUD was
confirmed correct and the 3D surfaces confirmed wrong, with no input automation at all.

## Files touched

- `Makefile` (`run` target)
- `docs/reference/` (screenshots)
- This document (validation log)

## Notes and risks

- **Do not call a story done on Dolphin alone** in the GX and audio epics. STORY-008's and
  STORY-012's acceptance criteria explicitly require hardware validation.
- A Wii test does not validate the GameCube (memory, no MEM2, no WPAD) or vice versa. If both
  targets are maintained, both must be tested at each milestone.
- Dolphin caches games in its list; in `-b -e` mode the `.dol` is re-read on every launch, which
  avoids the classic stale-binary trap.
