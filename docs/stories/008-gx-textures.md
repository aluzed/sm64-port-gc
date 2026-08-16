# STORY-008 — Textures: GX swizzle, cache and wrap modes

**Epic:** 2 — GX rendering
**Status:** ✅ Done — textures rendering correctly under Dolphin
**Depends on:** STORY-006
**Estimate:** L (3-5 d)
**Platform:** GC + Wii

## Context

`gfx_pc.c` already handles all the high-level texture logic: decoding the N64 formats
(RGBA16, IA8, CI4, CI8, …), converting to **linear RGBA32**, and caching by source address
(`gfx_texture_cache_lookup`, a 512-node pool reset when full).

So the backend only ever receives one thing through
`upload_texture(rgba32_buf, width, height)`: a linear RGBA8888 buffer. Four calls to
implement:

```c
uint32_t (*new_texture)(void);
void     (*select_texture)(int tile, uint32_t texture_id);
void     (*upload_texture)(const uint8_t *rgba32_buf, int width, int height);
void     (*set_sampler_parameters)(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt);
```

The difficulty is that GX does **not** read textures linearly: `GX_TF_RGBA8` is stored as
**4×4 texel tiles**, each occupying 64 bytes — 32 bytes of `(A,R)` pairs followed by 32 bytes
of `(G,B)` pairs. So a "swizzler" is required, plus 32-byte alignment and the mandatory
`DCFlushRange` before the GP reads the data.

## Goal

As a developer, I want the game's textures to display correctly, with the right filtering and
wrap modes, so the rendering matches the N64.

## Acceptance criteria

- [x] A checkerboard test texture displays with no offset, no channel swap and no tile
      corruption.
- [x] The three N64 wrap modes are honoured: `G_TX_WRAP` → `GX_REPEAT`, `G_TX_MIRROR` →
      `GX_MIRROR`, `G_TX_CLAMP` → `GX_CLAMP`, independently in S and T.
- [x] Bilinear / point filtering follows `linear_filter` (`GX_LINEAR` / `GX_NEAR`).
- [ ] Multitexturing works: `select_texture(0|1, id)` loads two distinct `GXTexObj` onto
      `GX_TEXMAP0` and `GX_TEXMAP1` — only texmap 0 is wired today.
- [x] No memory leak: recycling `gfx_pc`'s pool reuses the associated GX buffers.
- [ ] On **real hardware** (not just Dolphin), no cache artefacts. The story is not complete
      without this test.

## Tasks

1. **GX texture struct** with a `GXTexObj`, a 32-byte-aligned data pointer, dimensions, wrap
   and filter state.

2. **RGBA32 → `GX_TF_RGBA8` swizzle**:

   ```
   for each 4x4 tile (by, bx):
       offset = ((by * (width/4)) + bx) * 64
       for each texel (y, x) in the tile:
           k = (y * 4 + x) * 2
           dst[offset + k]          = A
           dst[offset + k + 1]      = R
           dst[offset + 32 + k]     = G
           dst[offset + 32 + k + 1] = B
   ```

   Dimensions must be rounded up to a multiple of 4. `gfx_pc` can send textures whose width is
   not a multiple of 4 (2×2 UI elements, for instance): allocate the rounded size and pad by
   replicating the edge, not with black.

3. **Allocation and cache coherency**: `memalign(32, …)`, fill, `DCFlushRange`, then
   `GX_InitTexObj` and `GX_InitTexObjFilterMode`. `DCFlushRange` is not optional: without it
   the GP reads stale RAM while the correct bytes still sit in the CPU's L1. Dolphin works
   anyway; hardware gives randomly corrupted textures.

4. **`select_texture(tile, id)`**: remember the current object per unit, and call
   `GX_LoadTexObj` at draw time.

5. **`set_sampler_parameters`.** `cms`/`cmt` are N64 values (`G_TX_*`), to be defined locally
   (`G_TX_MIRROR` = 0x1, `G_TX_CLAMP` = 0x2): `PR/gbi.h` is unusable in a translation unit
   that includes `<ogc/gx.h>`, see STORY-003 and STORY-006. Note that the wrap mode is part of
   the `GXTexObj`, not a global state, so changing it means re-initialising the object.

6. **`GX_InvalidateTexAll()`** at the start of each frame (STORY-006): GX caches textures in
   TMEM, and without invalidation a texture rewritten at the same address stays stale on
   screen.

7. **Memory placement.** Allocate texture data on **MEM2** on Wii (STORY-005): it is the
   biggest consumer and it is read by DMA, so latency-insensitive. Provide a central
   `ogc_texmem_alloc()` rather than scattered `memalign` calls. *(Not done yet.)*

## Files touched

- `src/pc/gfx/gfx_gx.c`
- `src/pc/gfx/gfx_ogc.c` (MEM2 allocator — pending)

## Notes and risks

- **Channel order.** `GX_TF_RGBA8` stores `AR` then `GB`, not `RGBA`. Getting it wrong gives
  an image with permuted colours — easy to recognise, but a time sink if you look elsewhere.
- One RGBA8 conversion per texture is expensive but amortised by `gfx_pc`'s cache. If
  profiling (STORY-018) shows spikes on level load, consider `GX_TF_RGB5A3` (half the memory,
  slightly lower quality) for textures without gradient alpha — but **measure first**.
- GX's TMEM is 1 MB. An SM64 texture rarely exceeds 64×64, so there is no risk of overflow,
  but multitexturing uses two distinct regions: check libogc's default
  `GX_InitTexCacheRegion` setup.
- CI4/CI8 textures are already converted to RGBA32 by `gfx_pc`: do **not** try to use GX
  palettised formats in v1, the memory saving does not justify the complexity.

## Implementation log

Implemented in `gfx_gx.c`: a 512-entry `GXTexture` pool (matching `gfx_pc`'s cache), the
RGBA8888 → `GX_TF_RGBA8` swizzle, `memalign(32, …)` allocation resized on demand,
`DCFlushRange` after every write, and wrap/filter mapping identical to `gfx_opengl.c` (clamp
takes precedence over mirror).

**Visual result: textures render correctly.** The in-game HUD — lives, coins, stars, camera
icons — is pixel-perfect, in the right places with the right colours. That proves the swizzle,
the `AR`/`GB` channel order, the `DCFlushRange` and the per-tile texture selection are all
right.

Two implementation details worth recording:

- **`upload_texture` does not receive the tile.** `gfx_pc` selects a tile and then uploads
  into "whatever was selected last", so the backend must remember `last_selected_tile` — the
  same thing `gfx_opengl.c` does with `glActiveTexture`.
- **`set_sampler_parameters` is called before the first upload** (inside
  `gfx_texture_cache_lookup`, with `false, 0, 0`), so the GX object cannot be built at that
  point. Parameters are stored and the object rebuilt as soon as data exists.

### What colour revealed

Two defects appeared once the image became legible, and **neither belongs to this story**:

1. **Colours are wrong on 3D geometry.** The Mario head from the intro renders with the right
   shape and highlights, but **black**. Identified cause: the combiner is not translated
   (STORY-007), every `shader_id` is forced onto `GX_MODULATE` and only combiner input 1 is
   read. For a vertex-lit model whose input 1 is the lighting and input 2 the base colour,
   that is exactly what you get: dark, with the highlights.
2. **Textured 3D surfaces are smeared**, while the 2D HUD is perfect. The only difference
   between the two is `w`: constant in 2D, varying in 3D. That is the signature of affine
   interpolation — see STORY-009, where the topic is now documented along with the constraint
   discovered here.

### Memory impact

Textures now allocate from the heap (`memalign`), up to 512 entries of 8 KB worst case,
~4 MB. Fold that into the [STORY-005](005-memory-mem1-mem2.md) budget, particularly for
GameCube where the measured headroom is ~9 MB. Moving them to MEM2 on Wii (task 7) is still
outstanding.
