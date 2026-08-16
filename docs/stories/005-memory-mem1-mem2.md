# STORY-005 — MEM1 / MEM2 memory map and binary footprint

**Epic:** 1 — Platform bring-up
**Status:** 🟡 Measured, GameCube verdict favourable — optimisation and instrumentation pending
**Depends on:** STORY-004
**Estimate:** M (2-3 d) — **a decision story, do it early**
**Platform:** GC + Wii

## Context

This is risk number one, and it has to be settled **before** sinking weeks into the GX backend.

Unlike the N64 (which streams assets from the cartridge over DMA), the port **embeds every
asset in the binary**: level geometry, decompressed textures, animations, audio sequences.
The `Makefile` defines `-DNO_SEGMENTED_MEMORY -DUSE_SYSTEM_MALLOC` and `main_pool_init()`
relies on the system `malloc`.

Available budgets:

| | GameCube | Wii |
|---|---|---|
| MEM1 (1T-SRAM, fast) | 24 MB | 24 MB |
| MEM2 (GDDR3, slower) | — | 64 MB (~52 usable in homebrew) |
| ARAM | 16 MB (DMA only, not CPU addressable) | 16 MB |

Out of the MEM1 budget also come: the `.dol` itself, the two XFBs (640×480×2 = 614 KB each
in 480i), the GX FIFO (256 KB–1 MB), the GX vertex buffer, the GX texture cache, the game's
main pool (`0x165000` ≈ 1.4 MB on N64), the effects pool (`0x4000`), and the newlib
stack/arena.

## Goal

As the port's architect, I want the real binary footprint and an explicit allocation plan, so
I can decide whether GameCube is reachable and size the GX caches knowingly.

## Acceptance criteria

- [x] A numbers table of the `.dol` sections (`.text`, `.data`, `.rodata`, `.bss`), versioned
      in this document *(version `us`; `eu` still to measure)*.
- [x] A target memory budget written out item by item, with the remaining headroom on Wii and
      on GameCube.
- [ ] The game boots on Wii with at least 8 MB of measured free memory at runtime.
- [x] The GameCube verdict is settled and documented: **reachable as is**, ≈ 9 MB of headroom
      — to be reconfirmed once STORY-008's texture cache is in place.
- [ ] An `ogc_print_mem_stats()` helper reports free memory on demand
      (`SYS_GetArena1Lo/Hi`, `SYS_GetArena2Lo/Hi`).

## Tasks

1. ~~**Measure.**~~ ✅ **Done** — `powerpc-eabi-size -A build/<target>/sm64.us.elf`, version
   `us`, dummy renderer, `-O2`, without `--gc-sections`:

   | Section | Wii | GameCube | Contents |
   |---|---|---|---|
   | `.init` + `.text` | 1,157,780 B | 1,135,936 B | code |
   | `.rodata` | 8,717,408 B | 8,716,688 B | **the assets** — 68 % of the binary |
   | `.data` | 2,760,820 B | 2,760,620 B | initialised data |
   | `.eh_frame` + `.eh_frame_hdr` | 201,152 B | 196,044 B | unwind tables — **useless** |
   | `.sdata` + `.sdata2` | 910 B | 862 B | small data (`r13`-relative) |
   | `.bss` + `.sbss` | 1,301,152 B | 1,278,880 B | not stored in the `.dol` |
   | **final `.dol`** | **12,838,432 B** | **12,810,432 B** | |
   | **static MEM1 footprint** | **≈ 13.5 MB** | **≈ 13.4 MB** | `.dol` + `.bss` |

   The prediction holds: `.rodata` dominates, and it is the embedded assets.

### GameCube verdict: ✅ favourable

MEM1 budget (24 MB) once the binary is loaded:

| Item | Size |
|---|---|
| Binary + `.bss` | 13.4 MB |
| 2 × XFB 640×480 | 1.17 MB |
| GX FIFO (256 KB) | 0.25 MB |
| **Remaining** | **≈ 9 MB** |

Nine megabytes for the game's main pool, the effects pool, the GX texture cache and the
stack is comfortable. **The GameCube target stays in scope**, with no need for ARAM tricks or
a resolution downgrade. To be reconfirmed after STORY-008, which is what will actually eat
into the rest.

On Wii, with 64 MB of MEM2 on top, the question does not arise.

### Immediate win identified

`.eh_frame` + `.eh_frame_hdr` are **~197 KB of dead weight**: stack unwind tables, useless for
C without exceptions. `-fno-asynchronous-unwind-tables` removes them. That is the first lever
to pull, before even `--gc-sections`.

2. **Instrument at runtime.** Add a helper in `gfx_ogc.c` printing `SYS_GetArena1Size()` and,
   on Wii, `SYS_GetArena2Size()` at boot and after game init.

3. **Put the big consumers on MEM2 (Wii).** libogc exposes arena 2 through
   `SYS_GetArena2Lo/Hi`. Put there, in priority order, the buffers that tolerate higher
   latency: the GX texture cache (STORY-008) and the audio buffers (STORY-012). Keep in MEM1:
   the GX FIFO, the vertex buffer, the game's main pool and the XFBs.

4. **Shrink the footprint** (measure before applying):
   - `-fno-asynchronous-unwind-tables`: ~197 KB, certain and risk-free. **Do this first.**
   - `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections` to drop dead code.
   - `-Os` on pure asset files (`levels/`, `actors/`, `bin/`): no speed to gain there, they
     are data tables.
   - Check that demo tables (`assets/demos`) and other versions' text are not embedded.

5. **GameCube mitigations**, only if measurement shows an overflow, by increasing cost:
   1. `--gc-sections` + `-Os` on assets (free, do it anyway);
   2. XFB at 320×240 instead of 640×480 (~900 KB, real visual cost);
   3. smaller GX texture cache with a more aggressive eviction policy;
   4. moving rarely used textures to **ARAM** with `ARAM_Init` and on-demand DMA — a proven
      technique but costly in complexity, last resort;
   5. dropping the GameCube target for v1.

6. **The game's main pool.** With `USE_SYSTEM_MALLOC`, `main_pool_init()` relies on `malloc`.
   Check the newlib arena is initialised first, and consider going back to the static pool to
   make the footprint **deterministic** and avoid fragmentation — a fixed pool is preferable
   on a machine without virtual memory.

## Files touched

- `Makefile` (`--gc-sections`, selective `-Os`)
- `src/pc/pc_main.c` (pool choice)
- `src/pc/gfx/gfx_ogc.c` (instrumentation)
- This document (measurements table)

## Notes and risks

- **Do not optimise blind.** The whole story rests on step 1: measure first.
- `--gc-sections` can drop symbols referenced only from assembly or from link-time pointer
  tables. Check the game still boots afterwards, and use `KEEP()` in the linker script if
  needed.
- MEM2 on Wii is **noticeably slower** than MEM1 (~2× the latency). Putting the game's main
  pool or the vertex buffer there would hurt performance: reserve MEM2 for DMA-consumed data.
- ARAM is **not** CPU addressable: anything living there must be DMA'd back before use. It is
  not transparent swap.
- Textures now allocate from the heap (STORY-008), up to ~4 MB worst case. Fold that into the
  budget above.
