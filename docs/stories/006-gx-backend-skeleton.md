# STORY-006 — GX backend skeleton (`GfxRenderingAPI`)

**Epic:** 2 — GX rendering
**Status:** ✅ Done — milestone M1 reached under Dolphin (`src/pc/gfx/gfx_gx.c`)
**Depends on:** STORY-004, STORY-005
**Estimate:** L (4-5 d)
**Platform:** GC + Wii

## Context

`gfx_pc.c` is a Fast3D display-list interpreter: it reads the game's RSP/RDP commands, handles
matrices, lighting and the texture cache, and **produces already-transformed triangles** which
it pushes into `buf_vbo[]` (floats, up to 26 per vertex) before calling
`rendering_api->draw_triangles()`.

In other words, transformation is already done on the CPU. The GX backend needs neither
matrices nor T&L — it receives vertices in homogeneous clip space. Excellent news: GX will
essentially act as a rasteriser and combiner.

The GX pipeline is however **not** a shader pipeline: it is a fixed-function state machine
with 16 TEV stages, 8 texture units and 8 texture coordinates. Every `glUniform`/`glDrawArrays`
becomes a sequence of `GX_Set*` plus `GX_Begin`/`GX_End`.

This story lays down the skeleton and the global GX state. The three hard pieces are split out
into STORY-007 (combiner), 008 (textures) and 009 (vertices).

## Goal

As a developer, I want a `GfxRenderingAPI` implementation on GX that initialises the pipeline,
manages render state (depth, blending, viewport, scissor) and draws a full-screen triangle, so
that milestone M1 is reached.

## Acceptance criteria

- [x] `gfx_gx_api` implements all 22 `GfxRenderingAPI` function pointers.
- [x] `z_is_from_0_to_1()` returns the right value for GX (**verified experimentally**).
- [x] A hardcoded triangle appears on screen, at the right coordinates, with the right colour.
- [x] `set_viewport` / `set_scissor` map `gfx_pc` coordinates (bottom-left origin, OpenGL
      style) onto GX's (top-left origin).
- [x] `set_depth_test`, `set_depth_mask`, `set_use_alpha` produce the expected GX state, and
      the state is cached to avoid redundant `GX_Set*` calls.
- [x] The end-of-frame sequence (`end_frame` → `finish_render`) lines up with
      `swap_buffers_*` from STORY-004.

## Tasks

1. **Create `src/pc/gfx/gfx_gx.c` / `gfx_gx.h`**, exporting `struct GfxRenderingAPI gfx_gx_api`.

2. **`init()`** — GX bring-up: FIFO (32-byte aligned), `GX_SetCopyClear`, viewport, scissor,
   `GX_SetDispCopySrc/Dst/YScale`, `GX_SetCopyFilter`, `GX_SetFieldMode`, `GX_SetPixelFmt`,
   `GX_SetCullMode`, `GX_SetDispCopyGamma`.

3. **Culling.** `gfx_pc` handles face orientation itself via `G_CULL_*` and flips vertex
   order. Start with `GX_CULL_NONE` (correct, slightly slower); only enable GX culling after
   visual validation.

4. **State cache.** Every `GX_Set*` writes into the FIFO; calling them per triangle saturates
   the bandwidth. Keep a mirror struct and only re-emit on an actual change.

5. **Depth and blending**: `set_depth_test`/`set_depth_mask` → `GX_SetZMode`;
   `set_use_alpha` → `GX_SetBlendMode`.

6. **Viewport / scissor with Y inversion.** `gfx_pc` thinks in OpenGL coordinates:
   ```c
   GX_SetViewport(x, cur_fb_height - y - h, w, h, 0.0f, 1.0f);
   ```
   Write that conversion **once**, in a helper, and comment it: it will be the source of half
   the visual bugs in this epic.

7. **Frame cycle**: `start_frame()` resets the state cache, `GX_InvVtxCache()`,
   `GX_InvalidateTexAll()`; `end_frame()` does `GX_DrawDone()`.

8. **`create_and_load_new_shader` / `lookup_shader` / `shader_get_info`.** "Shader" is
   misleading here: on GX it is a **TEV configuration**. Keep the same pool structure as the
   OpenGL backend.

## Files touched

- `src/pc/gfx/gfx_gx.c`, `src/pc/gfx/gfx_gx.h` (new)
- `src/pc/gfx/gfx_ogc.c`
- `src/pc/pc_main.c`
- `Makefile`

## Notes and risks

- **`gfx_gx.c` cannot include `PR/gbi.h`.** Verified in STORY-003: `<ogc/gx.h>` declares `Vtx`
  and `Mtx`, `PR/gbi.h` declares the same names with incompatible types, and the compiler
  refuses both in one translation unit. The backend only needs two GBI constants —
  `G_TX_MIRROR` (0x1) and `G_TX_CLAMP` (0x2), the only ones `gfx_opengl.c` uses — so define
  them locally with a comment pointing here.
- **The GX writes into a FIFO the GP consumes asynchronously.** Anything the GP reads
  (textures, vertex arrays) must be `DCFlushRange`d before use and 32-byte aligned. Missing
  that gives intermittent artefacts that are painful to diagnose and **do not reproduce under
  Dolphin** (the emulator is forgiving about caches). Corollary: test on real hardware early
  (STORY-017).
- `z_is_from_0_to_1()` feeds a computation in `gfx_pc.c`. Getting it wrong gives a game that
  displays but sorts depth incoherently. Test it explicitly.
- Do not try to use GX hardware T&L: `gfx_pc` already provides transformed vertices. Set
  `GX_SetNumChans` accordingly and pass colours straight through.

## Implementation log

`src/pc/gfx/gfx_gx.c` implements all 22 `GfxRenderingAPI` functions: state cache, depth,
blending, viewport and scissor with Y inversion, TEV configuration pool, triangle submission.
**Milestone M1 is reached**: the test triangle renders under Dolphin with correct Gouraud
interpolation, correct orientation, full screen.

Decisions taken along the way that differ from the plan above:

- **GX bring-up stays in `gfx_ogc.c`.** The plan was to move it here. In practice it depends
  on the video mode (`fbWidth`, `efbHeight`, `aa`, `vfilter`) and not on the renderer, and
  leaving it there is what keeps `ENABLE_GFX_DUMMY` builds showing something. `gfx_gx.c` owns
  render state only.
- **Projection: CPU-side perspective divide plus an identity orthographic projection.** The
  simplest option, chosen because affine interpolation is invisible without textures. To be
  revisited once textures land (STORY-009).
- **Textures gated** behind `GFX_GX_TEXTURES_IMPLEMENTED`. Sampling a `GX_TEXMAP0` that was
  never loaded makes every textured surface black — which in SM64 is nearly the whole screen,
  indistinguishable from "nothing draws at all".

### Three bugs only running could reveal

1. **The test triangle was hidden by the game.** Drawn in `start_frame`, it was covered by the
   full-screen black rectangle SM64 paints as its background. It now draws in `end_frame`, on
   top of everything. That cost several cycles of believing the pipeline drew nothing.

2. ~~**Inverted depth convention.**~~ **This entry was wrong — see the correction below.**
   It claimed that with `z_is_from_0_to_1()` returning `true`, `gfx_pc` supplies `z/w` with 0
   at the near plane, so the conversion had to be `z/w - 1` rather than `-(z/w)`. The
   reasoning was plausible and never measured, and it is the opposite of the truth.

3. **`guMtxIdentity()` on a `Mtx44`.** `Mtx` is `f32[3][4]`, `Mtx44` is `f32[4][4]` — both
   decay to `f32(*)[4]`, so **the compiler says nothing**, and the fourth row is left
   uninitialised. Harmless here (`GX_LoadProjectionMtx` in orthographic mode never reads it),
   but exactly the kind of time bomb that is never found later. Replaced with an explicit
   initialisation.

### The bring-up tooling: `-DGFX_OGC_BRINGUP_DEBUG`

Three aids, each of which paid for itself:

| Aid | What it buys |
|---|---|
| Blue clear colour | tells "the renderer draws nothing" apart from "the EFB→XFB copy is broken" |
| Test triangle in `end_frame` | isolates projection, viewport, vertex format, TEV and copy, independent of `gfx_pc` |
| Per-triangle false colours | makes geometry visible while everything renders white for lack of textures |

The third one confirmed the game really does submit geometry: the Mario head mesh from the
intro is clearly recognisable, one hue per triangle.

### Correction: the depth convention, measured

Bug 2 above was diagnosed by reasoning and got it backwards, which cost most of two later
sessions. **Measured** with `-DGFX_GX_DEBUG_DEPTH`, which paints `zn = z/w` as greyscale:

| Screen region | What it is | `zn` |
|---|---|---|
| bottom | floor, near the camera | **≈ 0.91** |
| top | background, far away | **≈ 0.33** |

So `gfx_pc` hands us `zn` that is **~1 at the near plane and ~0 at the far plane**. GX wants
−1 near and 0 far, so the mapping is a plain negation, `-(z/w)` — which is what the code did
before the "fix".

The inversion made the whole depth buffer run backwards. Its symptom was the entire scene
disappearing behind the sky, and switching the comparison to `GX_GEQUAL` hid that while
leaving the sort order wrong — a compensating error, not a fix. The code is now
`-(z/w)` with `GX_LEQUAL`, the canonical libogc setup, matching the `GX_MAX_Z24` clear.

The lesson is the same one as below, applied to a different subject: **a convention that can
be measured in one build should never be argued about.** The comment in `gfx_gx.c` now states
how to re-verify it in one run.

### Method lesson

Several cycles were lost on tests **confounded by write ordering**: painting the framebuffer
from the CPU inside `swap_buffers_end` necessarily overwrites the GX copy done in
`swap_buffers_begin`, which leads to concluding "the copy does not work" when it did. On a
pipeline where several stages write the same target, a bisection test must state *where* in
the frame it sits — otherwise it measures ordering, not function.
