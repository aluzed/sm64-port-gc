# STORY-009 — Vertex format and triangle submission

**Epic:** 2 — GX rendering
**Status:** 🟡 Base path landed with STORY-006; the projection trade-off is still open
**Depends on:** STORY-006, STORY-007
**Estimate:** M (2-4 d)
**Platform:** GC + Wii

> **Current state**: `gfx_gx_draw_triangles` decodes `buf_vbo`, submits in `GX_DIRECT`, and
> does the **perspective divide on the CPU** with an identity orthographic projection —
> option (A) of task 2. The depth conversion is `z/w - 1` (see STORY-006: the naive negation
> swaps near and far).
>
> **The predicted problem is confirmed** (STORY-008): with textures enabled, the 2D HUD is
> perfect while textured 3D surfaces are smeared. The only difference between them is `w`,
> constant in 2D and varying in 3D — the signature of affine interpolation. See "The trap in
> option (B)" below: the obvious remedy does not work as is.

## Context

`gfx_pc.c` accumulates triangles into a float array and then calls:

```c
void draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris);
```

The layout is **variable, depending on the current `shader_id`**. Reading `gfx_sp_tri1()`, the
per-vertex layout is exactly:

| Field | Condition | Floats |
|---|---|---|
| position `x, y, z, w` | always | 4 |
| texture coordinates `u, v` | `used_textures[0] \|\| used_textures[1]` | 2 |
| fog `r, g, b, factor` | `opt_fog` | 4 |
| input `n` (`rgb` or `rgba`) | for each `num_inputs` | 3 or 4 |

That is 4 to 26 floats per vertex. The batch is flushed every `MAX_BUFFERED = 256` triangles.

Positions are already **transformed into homogeneous clip space**; `w` is supplied for the
perspective divide, and `gfx_pc` has already adjusted `z` per `z_is_from_0_to_1()`.

## Goal

As a developer, I want to submit `gfx_pc`'s triangles to GX with a vertex descriptor matched
to the current `shader_id`, so the game's full geometry appears on screen.

## Acceptance criteria

- [x] Geometry displays complete, with no missing triangles or stray vertices.
- [ ] Texture coordinates are correct (no half-texel offset, no flipped V) — **currently
      broken on 3D surfaces**, see below.
- [ ] Perspective is correct: floor textures do not swim.
- [x] `MAX_BUFFERED` triangles are submitted in a single `GX_Begin`/`GX_End`.
- [x] No redundant `GX_SetVtxDesc`.

## Tasks

1. **Vertex descriptor**, configured from the loaded `shader_id`'s `CCFeatures`:
   ```c
   GX_ClearVtxDesc();
   GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
   GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
   GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
   GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
   if (uses_texture) { … GX_VA_TEX0, GX_TEX_ST, GX_F32 … }
   ```
   `GX_DIRECT` (data inline in the FIFO) is the simplest and enough for v1. Indexed arrays
   (`GX_INDEX16`) and display lists are an optimisation topic (STORY-018), not a correctness
   one.

2. **The `w` question.** GX does not accept an `XYZW` position — only `GX_POS_XYZ`. Two
   options:
   - **(A)** Divide on the CPU: `x/w, y/w, z/w`, with an orthographic projection downstream.
   - **(B)** Feed clip-space coordinates through a perspective projection and let the GP do
     the divide, which preserves perspective-correct texturing.

### The trap in option (B), found while analysing the render

Option (B) looks obvious: GX's perspective projection matrix has its last row pinned to
`(0, 0, -1, 0)`, so `w_clip = -z_view`. Feeding `(x_clip, y_clip, -w)` recovers the right `w`
and gives perspective correction for free.

But `z_clip = A·z_view + B`, so **`z_ndc = -A + B/w` depends only on `w`**: it is impossible to
carry both the game's `w` and its depth `z`. Two consequences:

- `A` and `B` must be chosen from a fixed near/far pair, which `gfx_pc` does not expose. With
  SM64's typical values (~100 / ~20000) the `[-1, 0]` range is well filled for 3D.
- **But 2D elements come out with `w = 1`**, giving `z_ndc = -A + B` ≈ −100: far outside
  `[-1, 0]`, so **entirely clipped**. Applying (B) naively makes the whole HUD disappear —
  which is precisely the one thing that renders correctly today.

Avenues to investigate, in order of preference:

1. **Per-batch projection.** Detect whether all the `w` values in the batch are equal: yes →
   2D content, keep (A); no → 3D, apply (B). One `GX_LoadProjectionMtx` per switch. It remains
   to be checked that depths from the two paths stay comparable — in practice 2D is drawn with
   depth testing off, which makes the question moot.
2. **Projective texture coordinates.** GX can divide `s/q`, `t/q` per pixel. That would need
   `1/w` routed to the texgen, which the two-component `GX_TEX_ST` vertex format does not
   allow directly.
3. Accept affine. **To be rejected**: SM64 has large floor triangles, it is very visible.

3. **Submission loop.** Decode `buf_vbo` with the current layout and emit in the order the
   descriptor requires: **position, then colour, then texture coordinates**, whatever the
   order in `buf_vbo`.

4. **Colour conversion.** `gfx_pc` gives floats in `[0,1]`; GX wants `u8`. Use
   `(u8)(f * 255.0f + 0.5f)` with clamping, not a plain cast (which truncates and visibly
   shifts gradients).

5. **Hoisting constant inputs.** Implement the scheme described in STORY-007 task 4: walk the
   first vertex's inputs, compare against the others, load the constants into TEV registers
   before `GX_Begin`.

6. **Texture coordinates.** `gfx_pc` already divides by `tex_width`/`tex_height` and applies
   the linear-filtering half-texel offset. Do **not** readjust anything on the GX side: if the
   image is offset, the cause is elsewhere (swizzle, wrap mode, or the rounded dimensions from
   STORY-008).

## Files touched

- `src/pc/gfx/gfx_gx.c`

## Notes and risks

- **`GX_Begin`/`GX_End` in direct mode writes into the FIFO through uncached accesses.** That
  works but is expensive: ~256 triangles × 3 vertices × ~40 bytes per frame. If profiling shows
  the CPU saturating, the answer is indexed array mode (STORY-018), not micro-optimising the
  loop.
- Do not try to reduce `MAX_BUFFERED`: it is a `gfx_pc.c` parameter (upstream code), and 256 is
  a good compromise.
- GX requires the vertex count declared to `GX_Begin` to match exactly what is emitted. A
  mismatch hangs the GP — symptom: a frozen image with no error message.
- Check face winding (`GX_SetCullMode`) once geometry is visible: if the inside of objects
  shows instead of the outside, that is inverted winding, not a depth bug.
