# STORY-011 — Video modes, resolution, PAL/NTSC and 16:9

**Epic:** 2 — GX rendering
**Status:** 🟡 Cadence, mode selection, 480p, 16:9 and the copy filter done; overscan **wired but
untuned** — it needs a real television, which no emulator can stand in for
**Depends on:** STORY-004, STORY-009
**Estimate:** M (2-3 d)
**Platform:** GC + Wii

> **Measured 2026-08-16 under Dolphin** (see [STORY-004](004-video-window-manager-backend.md)):
> with `VSYNCS_PER_FRAME = 2`, the Wii in NTSC 60 Hz runs at **29.97 fps** (correct), while the
> GameCube in PAL 50 Hz runs at **25.00 fps** — 17 % too slow, music and physics included.
> Task 3 below is therefore not optional.

## What landed beyond the cadence

**The picture's aspect is not the framebuffer's**, and conflating them was an active bug. PAL
576i renders 640x528, a ratio of 1.212, and the VI displays that as a full 4:3 frame; `gfx_pc`
computes `aspect_ratio = width / height` and so laid the HUD out for a screen narrower than the
one it was on, inset by about fifteen units a side. Every PAL console was affected, which is
most of them in Europe.

Fixed without disturbing the other backends: `gfx_pc_override_aspect_ratio()` (declared in a
dependency-free `gfx_pc_aspect.h`, because `gfx_pc.h` drags in the GBI and cannot be included
from a libogc file) is called from `gfx_ogc_start_frame`. The dimensions stay the real pixel
counts, since `gfx_pc` also uses them for the viewport and the scissor — only the ratio is
corrected.

**16:9** follows from the same place. On Wii the console knows, through
`CONF_GetAspectRatio()`. A GameCube has no such setting, so it is a `widescreen` line in the
config file. The frame stays 640x480 and the television stretches it, which is what GameCube
games did.

**480p** when all three conditions hold: a component cable is plugged in
(`VIDEO_HaveComponentCable()`), the user asked for progressive (`CONF_GetProgressiveScan()` on
Wii; on GameCube it is B held at boot, which the mode already reflects), and the mode has a
progressive counterpart. 50 Hz PAL has none, and asking for a mode the cable cannot carry is
how a console ends up showing nothing.

The refresh rate itself is never overridden. Forcing PAL60 on a console set to 50 Hz risks a
black screen on a 50 Hz-only television, and the cadence problem it would solve is already
solved on the clock.

## Context

The port hardcodes the logical resolution in `src/pc/gfx/gfx_screen_config.h`:

```c
#define DESIRED_SCREEN_WIDTH  640
#define DESIRED_SCREEN_HEIGHT 480
```

and the `Makefile` defines `-DWIDESCREEN` unconditionally for ports. `gfx_pc.c` uses
`RATIO_X` / `RATIO_Y` to fit the N64 viewport (320×240) to the target resolution.

On a console the resolution is not a free choice: it depends on the TV standard, the cable
plugged in and the system settings. The useful modes:

| Mode | Resolution | Rate | Context |
|---|---|---|---|
| NTSC 480i | 640×480 (interlaced) | 59.94 Hz | Americas/Japan, SCART/composite |
| PAL 576i | 640×528 | 50 Hz | Europe, PAL 50 mode |
| PAL60 / EURGB60 | 640×480 | 60 Hz | Europe, "60 Hz" setting on |
| 480p | 640×480 (progressive) | 59.94 Hz | component cable + progressive setting |

The critical problem is **PAL 50 Hz**: SM64 runs at N64's 30 fps (one game frame per two
fields at 60 Hz). At 50 Hz, a loop paced by `VIDEO_WaitVSync()` runs the game **17 % too
slowly** — music, physics and animation included.

## Goal

As a European or American player, I want the game to display at the right resolution, aspect
ratio and speed, whatever my TV and console settings.

## Acceptance criteria

- [ ] The video mode is derived from the system setting (`VIDEO_GetPreferredMode`), not
      hardcoded.
- [ ] In NTSC and PAL60, the game runs at nominal speed (time a known sequence).
- [ ] In PAL 50 Hz, game speed is correct **or** the 50 Hz mode is explicitly refused in favour
      of PAL60 with a clear message. The chosen behaviour is documented here.
- [ ] 480p is used automatically when available (`VIDEO_HaveComponentCable()`).
- [ ] Aspect ratio is correct in both 4:3 and 16:9: no stretching, no distorted UI.
- [ ] The UI is not cut off by overscan on a CRT.

## Tasks

1. **Mode selection.** Start from `VIDEO_GetPreferredMode(NULL)`, then consider
   `CONF_GetAspectRatio()` on Wii and `VIDEO_HaveComponentCable()`. Expose the real dimensions
   through `wm_api->get_dimensions()` so `gfx_pc` computes its ratios correctly.

2. **Make `gfx_screen_config.h` dynamic.** `DESIRED_SCREEN_WIDTH/HEIGHT` are compile-time
   macros used by `gfx_pc.c`; on a console the resolution is only known at boot. Two options:
   - keep 640×480 as the **logical** render resolution and let the VI rescale to the real mode
     (`GX_SetDispCopyYScale` already handles vertical scaling) — **recommended, no change in
     `gfx_pc.c`**;
   - or make the macros variable, which touches upstream code.

3. **Game cadence at 50 Hz.** Three options:
   - **(A)** Force PAL60/EURGB60 when the console allows it.
   - **(B)** Decouple the game loop from vsync: call `produce_one_frame()` at a logical 60 Hz
     driven by `gettime()`, and present at 50 Hz (dropping frames).
   - **(C)** Accept 50 Hz as the original PAL release did (which genuinely ran slower).

   **Revised decision after measurement: go with (B).** Option (A) was ruled out in STORY-004:
   forcing PAL60 from the backend overrides the console's own setting, at the risk of showing
   no picture at all on a 50 Hz PAL TV. Decoupling the game loop from the retrace is the only
   answer that stays correct in every mode.

   **✅ Implemented.** `swap_buffers_end` keeps the fixed two-retrace wait on 60 Hz modes —
   exactly 29.97 fps, exactly what the N64 did — and switches to time-based pacing when
   `VIDEO_GetCurrentTvMode()` reports `VI_PAL` or `VI_DEBUG_PAL`: burn whole retraces until
   1/30 s has elapsed. On a 50 Hz display that averages 30 game frames per second with a
   regular 2-2-1 retrace pattern.

   A deadline left far in the past — after a level load, say — is not chased: catching up
   would run the game fast, so the pacer resynchronises instead.

   **Measured on the GameCube target, which Dolphin emulates as PAL 50 Hz: 25.00 fps before,
   29.95 fps after.** That also un-starves the audio backend, which feeds the DMA one block per
   game frame and was running 17 % short.

4. **16:9.** On Wii, `CONF_GetAspectRatio()` gives the system setting. The `Makefile`'s
   `-DWIDESCREEN` already enables the field-of-view adjustment in `gfx_pc.c`. Make it a runtime
   choice rather than a compile-time one, or ship two builds. On GameCube there is no system
   setting: offer a config option (STORY-015).

5. **Overscan.** CRTs crop 5–10 % of the edges. libogc exposes `CONF_GetDisplayOffsetH()` on
   Wii. Provide an overscan setting (margin in pixels) applied to `GX_SetScissor` and the
   viewport, saved in the configuration.

6. **Anti-aliasing.** `rmode->aa` and `rmode->sample_pattern` come from the mode.
   `GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter)` enables the
   EFB→XFB copy filter, which smooths vertically — useful in interlaced modes to reduce
   flicker on thin UI lines.

## Where tasks 4, 5 and 6 actually stand

### Task 4 was already satisfied, by the header rather than by us

"Make it a runtime choice rather than a compile-time one" reads like outstanding work, and it
is not. With `WIDESCREEN` defined, the macros in `include/gfx_dimensions.h` are driven by the
**runtime** `gfx_current_dimensions.aspect_ratio`, and at 4:3 they reduce exactly to the
non-widescreen branch: `160 - 120 × (4/3) + v` is `v`.

So the `-DWIDESCREEN` build is a superset that follows whatever aspect the console reports —
`CONF_GetAspectRatio()` on Wii, `configWidescreen` on GameCube — and the unconditional
`-DWIDESCREEN` in the Makefile is what makes the runtime switch work at all. Turning it into an
option and defaulting it off would remove 16:9 support, not add a choice. Nothing to do, and
worth recording so it is not "fixed" later.

### Task 5, overscan: wired, deliberately inert by default

`overscan` is a configuration value in pixels, inset on every edge, saved with everything else.
It defaults to **0**, which draws exactly as before.

It is applied in `gfx_gx_set_viewport` and `gfx_gx_set_scissor` — the one place both rectangles
are converted — because `gfx_pc` reissues them every frame and anything set once at init is
overwritten by the first frame that draws. Every rectangle is scaled about the centre by the
same factor, so the HUD keeps its position relative to the scene and no caller has to know.

The picture shrinks rather than moving: a television crops both edges, so shifting the image
only trades a lost right edge for a lost left one. The alternative — moving the VI window with
`viWidth` / `viXOrigin` — keeps full resolution, but a wrong value there can leave a set with no
picture at all. This costs a little resolution and cannot black out a screen, which is the right
trade for a value nobody has been able to try yet.

**Untuned on purpose.** A value that suits one set is wrong on the next, and this story's own
note says an emulator cannot stand in for a television. The mechanism is there; the number is
the player's.

### Task 6 was already done

`GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter)` has been in
`gfx_ogc.c` since the video backend landed.

## Files touched

- `src/pc/gfx/gfx_ogc.c`
- `src/pc/gfx/gfx_gx.c` (EFB→XFB copy parameters; the overscan inset)
- `src/pc/configfile.c` / `configfile.h` (overscan, 16:9 on GC — see STORY-015)
- `Makefile` — nothing to do, see task 4 above

## Notes and risks

- Dolphin renders progressively by default and hides interlacing and overscan problems
  entirely. **This story cannot be validated on an emulator alone**; it needs a real TV,
  ideally one CRT and one flat panel.
- 480p requires the user to have a component cable **and** the progressive setting enabled.
  Detect it, do not assume it.
- Changing video mode after `GX_Init` requires reconfiguring the display copy. Choosing the
  mode **once at boot** simplifies things a lot.
