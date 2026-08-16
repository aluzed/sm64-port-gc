# STORY-018 — Optimisation: GX display lists, cache, GameCube support

**Epic:** 7 — Performance and polish
**Status:** To do
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
