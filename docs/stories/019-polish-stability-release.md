# STORY-019 — Polish: crash handler, clean exit, release

**Epic:** 7 — Performance and polish
**Status:** 🟡 **the exit paths are done** (tasks 1–3), not yet run on hardware; the exception
handler, the leak hunt and the release itself remain
**Depends on:** STORY-016, STORY-017, STORY-018
**Estimate:** M (2-3 d)
**Platform:** GC + Wii

## Context

The game is playable and fast. What is left is what separates a prototype from something you
hand to someone else: not forcing the user to unplug their console.

On a console there is no window manager to fall back on. A program that ignores HOME, RESET and
POWER **forces a physical power cut** — unacceptable for public distribution, and painful even
during development (which is why the stubs were put in place back in STORY-004).

## Goal

As a user, I want to quit the game cleanly, power the console off normally, and get actionable
information when it crashes, so the port is usable day to day.

## Acceptance criteria

- [x] **HOME** (Wiimote) or **Start+X+Y** / the RESET button (GameCube) exits the game, saves
      the configuration and returns to the Homebrew Channel / the loader. *Written; not yet run
      on hardware.*
- [x] The console's **POWER** button powers off cleanly (save config, then
      `SYS_ResetSystem(SYS_POWEROFF, …)`). *The save was the bug; see below.*
- [x] The console's **RESET** button restarts cleanly. *Written; not yet run on hardware.*
- [ ] On a CPU exception, a readable screen shows the exception type, the faulting address and
      a call stack — instead of a silent freeze.
- [ ] No detectable memory leak over a 30-minute session with repeated level changes (free
      memory stable, measured with the STORY-005 instrumentation).
- [ ] A `v0.1.0` release is tagged, with release notes listing what works and what does not.

## Tasks

1. **System callbacks**, in `gfx_ogc.c` (completing the stubs from STORY-004):
   ```c
   static volatile bool should_quit = false;
   static void on_reset(u32 irq, void *ctx) { should_quit = true; }
   static void on_power(void)               { should_quit = true; power_off = true; }

   SYS_SetResetCallback(on_reset);
   SYS_SetPowerCallback(on_power);      /* Wii only */
   ```
   These run in interrupt context: **raise a flag and nothing else**. Never write to the SD or
   call GX code from them.

2. **Clean exit.** Turn the `main_loop` (STORY-004) into `while (!should_quit) …` followed by
   `ogc_shutdown()`: save the configuration (STORY-015), `AUDIO_StopDMA()`, `GX_AbortFrame()`,
   `VIDEO_SetBlack(TRUE)`, `VIDEO_Flush()`, `VIDEO_WaitVSync()`, then `SYS_ResetSystem(...)` or
   `SYS_POWEROFF` depending on the flag.

   On a Wii launched from the Homebrew Channel, `exit(0)` usually returns to the channel (libogc
   installs a return stub); keep `SYS_ResetSystem` as the fallback when the stub is absent.

3. **HOME button.** On Wii, `WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME` triggers the exit. The
   GameCube has no equivalent: pick a deliberately awkward combination (Start + X + Y held for
   1 s) to avoid accidental exits mid-game. Document it in the README.

4. **Exception handler.** libogc ships a default exception display, but the framebuffer has to
   be in a usable state. At minimum, show SRR0 (the faulting address) and DSISR. With the `.map`
   produced by STORY-002, the address translates to a function name via
   `powerpc-eabi-addr2line -e sm64.us.elf <address>`. Document that procedure in the README —
   it is what makes user bug reports actionable.

5. **Leak hunt.** Instrument the GX backend's allocations (textures above all) and print free
   memory at every level change. `gfx_pc.c`'s texture cache resets abruptly when its pool fills
   (`gfx_texture_cache.pool_pos = 0`): **check that the associated GX buffers are reused and
   not reallocated**, or a leak is guaranteed on every cache rotation. That is the most likely
   leak site in the whole port.

6. **`v0.1.0` release notes.** List honestly:
   - what works (platforms, controllers, saving, measured performance);
   - what does not or is degraded (combiner noise, 50 Hz, GameCube per the STORY-018 verdict,
     rumble);
   - the supported game versions (`us`, `eu`, …);
   - the validated devkitPPC/libogc versions;
   - the bug-report procedure with `addr2line`.

7. **Documentation review.** Check that `README.md` takes someone new from zero to a running
   game with no prior knowledge of the port.

## Done so far — the exit paths (tasks 1–3)

### Task 1 was already in place

The RESET and POWER callbacks, and the `should_quit` flag, landed with STORY-004 and raise a
flag and nothing else, as required.

### A bug the task list did not predict: POWER lost the configuration

`gfx_ogc_shutdown` called `SYS_ResetSystem(SYS_POWEROFF, …)` **before** `exit(0)`, and
`SYS_ResetSystem` does not return. `atexit(save_config)` therefore never ran on that path. RESET
and HOME saved correctly; the POWER button silently discarded whatever had changed since the
last save.

The fix is an ordering one, and the ordering is the whole point. Powering off moves into
`gfx_ogc_finish_shutdown()`, which `pc_main` registers with `atexit` **before** it registers
`save_config` — `atexit` fires handlers last-registered-first, so the config is written and the
console powers off after. Shutdown itself now always ends in `exit(0)`.

### Task 2: the engines that keep running on their own

Shutdown now calls `audio_ogc_stop()` and `GX_AbortFrame()` before blanking the screen. There is
no OS here to reclaim a device: the audio DMA keeps walking its buffer and the GP keeps
consuming a FIFO whose memory the next program is about to use.

`audio_ogc_stop()` is deliberately **not** part of `AudioAPI`. That interface is shared with
every other platform and none of them needs it — their process ends and the OS cleans up. It
unregisters the DMA callback before stopping the engine, because the callback re-arms from
whatever is still queued and stopping first leaves a window where the interrupt undoes the stop.

### Task 3: the way out

- **Wii** — HOME, on the Remote or the Classic Controller. Read independently of the arbitration
  and of whether the peripheral was accepted at all, so that a player holding a Remote with no
  Nunchuk — who cannot play — can still leave without pulling the plug.
- **GameCube** — Start + X + Y held for one second. Timed on the clock rather than counted in
  frames: the frame rate is 30 or 25 depending on the video mode, and "one second" should not
  mean 1.2 s on a PAL console. X and Y are unmapped in SM64, so the pair is free; Start is
  suppressed while both are held, or every attempt to quit would open the pause menu on the way
  out.

Documented in `README.md` under *Quitting*.

## What is not done

- **Task 4, the exception handler.** Untouched.
- **Task 5, the leak hunt.** Untouched. The story names the likely site — `gfx_pc.c` resetting
  `gfx_texture_cache.pool_pos` to 0 without releasing the GX buffers behind it — and that is
  still the place to look first.
- **Tasks 6 and 7, the release.** Correctly blocked: the story's own note says not to tag until
  STORY-017 records a hardware validation, and none of this has run on a console.

## Files touched

- `src/pc/gfx/gfx_ogc.c` — shutdown sequence, the `atexit` finisher, a millisecond clock
- `src/pc/gfx/gfx_ogc.h` — the finisher and the clock
- `src/pc/pc_main.c` — `atexit` registration order
- `src/pc/audio/audio_ogc.c` / `.h` — `audio_ogc_stop()`
- `src/pc/controller/controller_ogc.c` — the GameCube exit combination
- `src/pc/controller/controller_wpad.c` / `.h` — HOME
- `README.md` — the *Quitting* section
- `src/pc/gfx/gfx_gx.c` (texture release) — still to do, task 5
- `docs/stories/README.md` (status updates)

## Notes and risks

- POWER and RESET callbacks run in interrupt context with a reduced stack. Calling `fopen()` or
  GX code from them freezes the console in a particularly confusing way.
- `SYS_ResetSystem(SYS_RETURNTOMENU)` returns to the Wii system menu, not to the Homebrew
  Channel. Getting back to the HBC is `exit(0)` with the loader's return stub installed. Test
  both paths; the behaviour depends on the loader used.
- Do not tag a release until STORY-017 records a real-hardware validation of the corresponding
  milestone.
