# STORY-011 — Video modes, resolution, PAL/NTSC and 16:9

**Epic:** 2 — GX rendering
**Status:** To do — ⚠️ **the 50 Hz half is a confirmed correctness bug, not polish**
**Depends on:** STORY-004, STORY-009
**Estimate:** M (2-3 d)
**Platform:** GC + Wii

> **Measured 2026-08-16 under Dolphin** (see [STORY-004](004-video-window-manager-backend.md)):
> with `VSYNCS_PER_FRAME = 2`, the Wii in NTSC 60 Hz runs at **29.97 fps** (correct), while the
> GameCube in PAL 50 Hz runs at **25.00 fps** — 17 % too slow, music and physics included.
> Task 3 below is therefore not optional.

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

   Sketch: `swap_buffers_end` accumulates elapsed time (`gfx_ogc_get_ticks()`) and only calls
   `produce_one_frame()` once 1/30 s has passed, presenting on every retrace. At 50 Hz that
   gives a correct game cadence with a 2-2-1 frame distribution (a slight, regular judder); at
   60 Hz the behaviour is identical to today's. Keep `VSYNCS_PER_FRAME` as the simple path for
   60 Hz modes.

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

## Files touched

- `src/pc/gfx/gfx_ogc.c`
- `src/pc/gfx/gfx_gx.c` (EFB→XFB copy parameters)
- `src/pc/configfile.c` / `configfile.h` (overscan, 16:9 on GC — see STORY-015)
- `Makefile` (conditional `-DWIDESCREEN`)

## Notes and risks

- Dolphin renders progressively by default and hides interlacing and overscan problems
  entirely. **This story cannot be validated on an emulator alone**; it needs a real TV,
  ideally one CRT and one flat panel.
- 480p requires the user to have a component cable **and** the progressive setting enabled.
  Detect it, do not assume it.
- Changing video mode after `GX_Init` requires reconfiguring the display copy. Choosing the
  mode **once at boot** simplifies things a lot.
