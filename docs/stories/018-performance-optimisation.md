# STORY-018 — Optimisation: GX display lists, cache, GameCube support

**Epic:** 7 — Performance and polish
**Status:** 🟡 **Task 1 built** — the instrumentation exists and the table below is waiting for a
session on hardware. No optimisation attempted, by design: task 1 gates the rest.
**Depends on:** STORY-010, STORY-012, STORY-014
**Estimate:** L (4-6 d)
**Platform:** GC + Wii

## Context

By this point the game runs. This story makes it **smooth**, and settles the fate of the
GameCube target left open by STORY-005.

Hardware comparison points:

| | N64 | GameCube | Wii |
|---|---|---|---|
| CPU | R4300i 93 MHz | Gekko 486 MHz | Broadway 729 MHz |
| GPU | RCP 62 MHz | Flipper 162 MHz | Hollywood 243 MHz |

The hardware is far ahead of the N64. If the game does not hold 30 fps, the cause is an
inefficiency in the port, not a hardware limit. Suspects, by likelihood:

1. **`GX_DIRECT` vertex submission** (STORY-009): every vertex goes through uncached writes
   into the FIFO. With ~256 triangles per batch and several batches per frame, this is the
   first CPU cost.
2. **Redundant GX state changes**: every `GX_Set*` writes into the FIFO; without an effective
   state cache we emit thousands per frame.
3. **Texture conversion** on level load: latency spikes visible as hitches when entering a
   course.
4. **`gfx_pc.c` itself**: a display-list interpreter running on the CPU. On a 486 MHz Gekko
   that is not negligible.

## Goal

As a player, I want a stable 30 frames/s in every scene, so the experience matches or beats the
original.

## Acceptance criteria

- [ ] Stable 30 fps on Wii in the 8 reference scenes from STORY-017, including the heaviest
      (Bob-omb summit, a filled Wet-Dry World).
- [ ] No hitch longer than 100 ms on level load.
- [ ] An fps counter toggled by a config option, also reporting CPU and GP time separately.
- [ ] The GameCube verdict is final: the target either works and is hardware-validated, or it
      is dropped from v1 with the reasons documented.
- [ ] No optimisation applied without a prior measurement, and every gain quantified in the
      table below.

## Tasks

1. **Instrument before optimising.** Measure per frame:
   - total CPU time (`gettime()` around `produce_one_frame`);
   - time in `gfx_pc` (interpretation) vs in the GX backend (submission);
   - time waiting on `GX_DrawDone()` — if high, the GP is the bottleneck, not the CPU;
   - time waiting on `VIDEO_WaitVSync()` — if high, all is well.

   Record the measurements here:

   | Scene | CPU (ms) | GP (ms) | VSync wait (ms) | fps |
   |---|---|---|---|---|
   | Castle lobby | | | | |
   | Bob-omb (summit) | | | | |
   | … | | | | |

   ### Built — how to take the measurement

   Set `perf_log = true` in `sm64config.txt`, play, and **quit through HOME or Start + X + Y**.
   The log lands next to the save as `perf.log`.

   It is a log and not an on-screen counter for a reason the acceptance criteria did not
   anticipate: there is no way to draw text anywhere in `src/pc`, and adding one means touching
   the render path — the same wall STORY-014's Nunchuk message hit. A log is better here
   regardless. The numbers wanted are from a thirty-minute session across eight scenes, which is
   not something anyone can read off a corner of the screen while playing.

   One line per second of play, microseconds unless noted:

   ```
   # sec frames fps frame_avg frame_max cpu_avg submit_avg submit_max
   #     gp_wait_avg gp_wait_max vsync_avg vsync_max
   ```

   How to read it, which is the whole point of measuring before optimising:

   - **`vsync_avg` large** — the console is idle waiting for the retrace. There is headroom and
     nothing to optimise.
   - **`gp_wait_avg` large** — the CPU is blocked in `GX_DrawDone` waiting for the GP. The
     bottleneck is the GP, and no amount of CPU work will move it. Suspects 1 and 4 in the list
     above are then the wrong tree.
   - **`submit_avg` large** — the CPU is busy filling the FIFO. That is suspect 1, and
     optimisation #2 (indexed vertex arrays) is the answer.
   - **`cpu_avg` large with the other three small** — the CPU is busy somewhere else, which
     points at `gfx_pc` interpretation, suspect 4.
   - **`frame_max`** — the hitch column, and the one that answers "no hitch longer than 100 ms".

   The buffer holds 2048 seconds, about 34 minutes, and covers the session STORY-019 asks for.
   Past that the oldest seconds are overwritten and **the log says so in a header line**: a
   silently truncated measurement is worse than a short one.

   Off by default and allocated lazily, so a normal build carries neither cost nor footprint.
   It costs 2.4 KB of binary.

   Two caveats worth stating before anyone reads a number from it:

   - **Dolphin figures are meaningless here**, as this story already says. It runs on an x86
     hundreds of times faster than a Gekko.
   - **The log only exists if the game is quit politely.** Writing it during play would stall the
     frame loop and corrupt the very thing being measured, so it is written on the shutdown path.
     That is why STORY-019's exit work had to come first — and why pulling the plug loses it.

2. **Optimisation #1 — vertex submission.** If measurement confirms the diagnosis, replace
   direct-mode `GX_Begin` with an **indexed vertex array**:
   ```c
   GX_SetArray(GX_VA_POS,  vtx_pos_buffer, stride);
   GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
   ```
   The CPU writes into an aligned buffer, `DCFlushRange`s once, and the GP reads by DMA. A
   large expected win on 256-triangle batches.

   The more aggressive variant is compiling batches into **GX display lists**
   (`GX_BeginDispList` / `GX_EndDispList`). That only helps for geometry re-emitted unchanged,
   which is not the case here since `gfx_pc` regenerates everything every frame. **Reject**
   unless a finding says otherwise.

3. **Optimisation #2 — quantise attributes.** `GX_F32` positions and texture coordinates cost
   12 and 8 bytes per vertex. Moving texture coordinates to `GX_S16` with a fixed shift
   (`frac` in `GX_SetVtxAttrFmt`) halves the volume. Only worth it if FIFO bandwidth is shown
   to be the limit.

4. **Optimisation #3 — state cache.** Audit how many `GX_Set*` calls are actually emitted per
   frame (an instrumentation counter). Any call whose parameter matches the previous one is
   pure waste.

5. **FIFO size.** `DEFAULT_FIFO_SIZE` is 256 KB. If the CPU regularly waits for the GP to drain
   the FIFO, growing it (512 KB – 1 MB) improves CPU/GP overlap. Trade against the memory budget
   (STORY-005), especially on GameCube.

6. **Cache and alignment.** Check that the hot structures in `gfx_pc.c` (`buf_vbo`, the texture
   cache) are 32-byte aligned: the Gekko has 32-byte cache lines, and a misaligned array
   doubles the misses. `buf_vbo` is `256 × 26 × 3 × 4` ≈ 80 KB — it will not fit in L1 (32 KB),
   but its sequential walk is well predicted by the prefetcher.

7. **Compiler options.** Try `-O3` and `-funroll-loops` on `gfx_pc.c` and `gfx_gx.c` **only**
   (not globally: no gain on asset files and the footprint grows). The Gekko has
   **paired-single** instructions (SIMD over two floats) that GCC rarely uses automatically;
   hand-using them in `gfx_matrix_mul` is possible but intrusive — **last resort**.

8. **GameCube verdict.** Redo the STORY-005 measurements with the final binary:
   - does the `.dol` fit in MEM1 alongside the GX buffers and the game pool?
   - does the 486 MHz Gekko hold 30 fps after optimisations 2 to 4?

   If yes: validate on hardware and keep the target. If no: document precisely what blocks and
   drop the target from v1 rather than shipping something unplayable.

## Files touched

- `src/pc/gfx/gfx_perf.c` / `.h` — **new**, the frame-timing log
- `src/pc/configfile.c` / `.h` — the `perf_log` option
- `src/pc/pc_main.c` — initialisation, after the config and the storage mount
- `src/pc/gfx/gfx_gx.c`
- `src/pc/gfx/gfx_ogc.c` (instrumentation, fps counter)
- `src/pc/gfx/gfx_pc.c` (alignment, only if measured necessary)
- `Makefile` (per-file flags)

## Notes and risks

- **Measure first.** Every hypothesis in this story is plausible and none is verified. Task 1
  gates the rest; skipping instrumentation means optimising at random.
- Dolphin gives **no useful performance signal**: it runs on an x86 hundreds of times faster
  than the Gekko. Every performance measurement must be taken on hardware.
- The original N64 game ran at 30 fps with drops. Reproducing the drops is not a goal: a
  constant 30 fps is better than the original and enough. The 60 fps patch in `enhancements/`
  is **out of scope**.
- Optimising `gfx_pc.c` breaks compatibility with upstream. Touch it last, and isolate the
  changes to keep rebases cheap.
