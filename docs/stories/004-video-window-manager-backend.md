# STORY-004 — Window-manager backend: VIDEO/VI init, framebuffers, main loop

**Epic:** 1 — Platform bring-up
**Status:** ✅ Implemented and validated under Dolphin — real hardware pending (`src/pc/gfx/gfx_ogc.c`)
**Depends on:** STORY-003
**Estimate:** S (1-2 d)
**Platform:** GC + Wii

## Context

`GfxWindowManagerAPI` abstracts the window, the event loop and presentation. On a console
there is neither a window nor an event manager, so the implementation is far simpler than
`gfx_sdl2.c` — but it carries two key responsibilities: VI/framebuffer setup and **frame
pacing**.

## Goal

As a developer, I want a `gfx_ogc_wm_api` backend that brings up the libogc video subsystem,
allocates the external framebuffers (XFB) and drives the game loop at the refresh rate, so
the game runs at the right speed before there is anything to draw.

## Interface mapping

Implemented in `src/pc/gfx/gfx_ogc.c`:

| Member | Console implementation |
|---|---|
| `init` | `VIDEO_Init`, preferred mode, XFB allocation, `VIDEO_Configure` |
| `set_keyboard_callbacks` | no-op |
| `set_fullscreen_changed_callback` / `set_fullscreen` | no-op |
| `main_loop` | loop: `run_one_game_iter()` |
| `get_dimensions` | current `GXRModeObj` dimensions |
| `handle_events` | no-op (controllers are polled by `ControllerAPI`) |
| `start_frame` | returns `true` |
| `swap_buffers_begin` | `GX_CopyDisp` + `GX_DrawDone` |
| `swap_buffers_end` | `VIDEO_SetNextFramebuffer`, `VIDEO_Flush`, `VIDEO_WaitVSync`, XFB flip |
| `get_time` | `ticks_to_microsecs(gettime())` as seconds |

## Acceptance criteria

- [x] At boot the screen goes from the boot black to the `GX_SetCopyClear` background colour,
      proving VI, XFB and the loop are alive.
- [x] XFB double buffering: two framebuffers from `SYS_AllocateFramebuffer`, alternated on
      each `swap_buffers_end`.
- [x] `get_time()` returns a monotonic time in seconds.
- [x] `produce_one_frame()` runs exactly once per game frame (measured: 29.97 fps in NTSC).
- [ ] No tearing on hardware — pending.
- [x] No libogc assert and no exception screen, on Dolphin at least.

## Tasks

1. Create `src/pc/gfx/gfx_ogc.h` / `gfx_ogc.c` exporting `struct GfxWindowManagerAPI gfx_ogc_wm_api`.

2. **Video init** (canonical libogc sequence):
   ```c
   VIDEO_Init();
   rmode = VIDEO_GetPreferredMode(NULL);
   xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
   xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
   VIDEO_Configure(rmode);
   VIDEO_SetNextFramebuffer(xfb[0]);
   VIDEO_SetBlack(FALSE);
   VIDEO_Flush();
   VIDEO_WaitVSync();
   if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
   ```
   `VIDEO_GetPreferredMode` honours the console's own settings (PAL/NTSC, 480p when a
   component cable is present). Fine mode selection is STORY-011.

3. **`main_loop`** is paced by `VIDEO_WaitVSync()` inside `swap_buffers_end`, which blocks
   until retrace. No timers, no sleeps.

4. **Clock**: `gettime()` returns time-base ticks; `ticks_to_microsecs()` converts. Subtract
   the start tick first to avoid precision loss in `double`.

5. **Split of responsibility with the GX backend.** The API separates `swap_buffers_begin`
   (end of rendering) from `swap_buffers_end` (presentation).

6. **Clean exit**: wire `SYS_SetResetCallback` / `SYS_SetPowerCallback` so RESET and POWER do
   not freeze the console (detailed in STORY-019, but stubbing it here avoids having to
   unplug the console on every test).

## Files touched

- `src/pc/gfx/gfx_ogc.c`, `src/pc/gfx/gfx_ogc.h` (new)
- `src/pc/pc_main.c`
- `Makefile`

## Notes and risks

- `MEM_K0_TO_K1` is mandatory: the XFB must be accessed **uncached**. Forgetting it gives a
  corrupted or frozen display — a classic and confusing symptom.
- On Wii, `VIDEO_GetPreferredMode` may return an anamorphic 16:9 mode. Not a concern here —
  that is STORY-011.
- `VIDEO_WaitVSync()` locks the game to the display refresh. In PAL 50 Hz the game runs 17 %
  too slow unless something is done — a known port problem, handled in STORY-011 / 019.
- Do not allocate XFBs with `malloc`: `SYS_AllocateFramebuffer` handles the alignment and
  exact size the VI requires.

## Implementation log

### Frame pacing: the bug only running could reveal

First run under Dolphin: **59.94 fps**. Everything worked — VI, XFB, GX, loop — but the game
was running at **double speed**.

SM64 is a 30 fps game: on N64 one frame is produced every **two** VI retraces, and the PC
backends explicitly cap themselves at `1000 / 30` ms (`sync_framerate_with_timer` in
`gfx_sdl2.c`, and `1000000000 / 30` in `gfx_dummy.c`). Presenting on every retrace speeds up
**everything**: logic, physics, animation and audio.

Fixed with `VSYNCS_PER_FRAME = 2` in `swap_buffers_end`. Measured afterwards: **29.97 fps**,
the exact N64 cadence.

A useful reminder for what follows: `VIDEO_WaitVSync()` paces the loop on the **retrace**,
not on the game's frame rate. The ratio between the two depends on the video mode, which
leads straight to the next point.

### The PAL corollary, measured

| Target | Mode Dolphin emulates | Retraces/s | Measured fps | Verdict |
|---|---|---|---|---|
| Wii | NTSC 60 Hz | 59.94 | **29.97** | ✅ correct |
| GameCube | PAL 50 Hz | 50.00 | **25.00** | ❌ 17 % too slow |

The code does exactly what it is told in both cases; it is the 50 Hz mode that is
incompatible with a game designed for 60. The problem anticipated by
[STORY-011](011-video-modes-resolution.md) is therefore experimentally confirmed, and **it is
not a polish item but a correctness bug**: at 25 fps the music and the physics are wrong.

Deliberately **not fixed here**: forcing PAL60 from `gfx_ogc.c` would override the console's
own setting, at the risk of showing no picture at all on a 50 Hz PAL TV. The right answer is
to decouple the game loop from the retrace, which is STORY-011's scope.

### Validated

- Clean boot, **no exception** in the Dolphin log, on both targets.
- XFB double buffering, `VIDEO_WaitVSync()`, `GX_CopyDisp`: working.
- `produce_one_frame()` cadence exact at 60 Hz.
- 30 s of continuous running with no drift or crash.

Still open for lack of hardware: absence of tearing, `get_time()` precision, and RESET /
POWER button behaviour.
