# STORY-010 — Effects: fog, noise, alpha compare, Z decals

**Epic:** 2 — GX rendering
**Status:** To do — ⬅️ **next** (task 0 is resolved; the four effects remain)
**Depends on:** STORY-007, STORY-008, STORY-009
**Estimate:** M (2-3 d)
**Platform:** GC + Wii

## Task 0 — ✅ resolved: the depth mapping was inverted

Inherited from [STORY-009](009-vertex-format-draw-triangles.md) and now fixed. **The whole
scene renders, in the right order.**

The root cause was in STORY-006: the sign of the depth mapping was decided by reasoning rather
than measurement, and got it backwards. Measured with `-DGFX_GX_DEBUG_DEPTH`, `gfx_pc` hands
the backend `zn = z/w` that is **~1 at the near plane and ~0 at the far plane** (bottom of
screen 0.91, distant background 0.33). GX wants −1 near and 0 far, so the mapping is a plain
negation `-(z/w)`, not `z/w - 1`.

An intermediate "fix" switched the comparison to `GX_GEQUAL`. It made the scene appear, but it
was a **compensating error**: it inverted the comparison to match an inverted buffer, which
left the sort order wrong. Both are now correct — `-(z/w)` with `GX_LEQUAL`, the canonical
libogc setup matching the `GX_MAX_Z24` clear. `-DGFX_GX_DEBUG_ZFLIP` swaps the comparison to
re-check the orientation in one run.

A second, independent fix was needed along the way: **the depth mask has to be gated on the
depth test.** OpenGL writes nothing to the depth buffer when `GL_DEPTH_TEST` is disabled,
whatever `glDepthMask` says; GX treats the comparison as always passing and still honours
`update_enable`, so it does write. `gfx_pc` drives both flags straight from the N64 render
mode, so the difference shows up immediately.

The decal task below can now build on a mapping that has been measured rather than assumed.

## Context

Four combiner options, carried by the high bits of `shader_id`, remain once base rendering
works:

| Option | Bit | What the OpenGL backend does |
|---|---|---|
| `SHADER_OPT_FOG` | 25 | final `mix(colour, fog.rgb, fog.a)` with a per-vertex factor |
| `SHADER_OPT_TEXTURE_EDGE` | 26 | `discard` when `alpha < 0.3` (crisp cutout edges) |
| `SHADER_OPT_NOISE` | 27 | pseudo-random dither based on frame number and scanline |
| — | — | `set_zmode_decal()`: depth offset for decals (shadows, footprints) |

Each has a GX equivalent, but none is a direct translation: these are four independent little
problems.

## Goal

As a player, I want distance fog, crisp cutout silhouettes, shadows on the ground and correct
fade transitions, so the rendering is faithful to the original game.

## Acceptance criteria

- [ ] **Fog**: the haze in Bob-omb Battlefield and Shifting Sand Land renders with the right
      colour and the right distance falloff.
- [ ] **Texture edge**: grates, foliage and cutout signs have crisp edges, with no halo or
      semi-transparent fringe.
- [ ] **Z decal**: Mario's shadow and footprints sit on the ground with no z-fighting and no
      visible detachment, at any camera distance.
- [ ] **Noise**: fades to black and level transitions show no hard banding. If noise is
      deferred, the degradation must be a smooth fade, not a visible artefact.
- [ ] None of these options enables an expensive path when the `shader_id` does not ask for it.

## Tasks

### 1. Fog

GX has a hardware fog unit (`GX_SetFog`), but it computes the factor from screen Z — whereas
`gfx_pc` supplies an already-computed **per-vertex factor** (`v_arr[i]->color.a` reused as the
factor) and a per-vertex fog colour.

Two approaches:
- **(A) An extra TEV stage**: pass the fog factor in `GX_VA_CLR1`'s alpha channel and the fog
  colour in a constant TEV register, then add a final stage
  `lerp(computed_colour, fog_color, fog_factor)` — exactly TEV's native operation.
  **Recommended**: faithful to the reference behaviour, costs one stage out of 16.
- **(B) Hardware `GX_SetFog`**: cheaper but computes the factor differently from `gfx_pc`, so
  the rendering differs. Rejected for v1.

Note that `gfx_pc` forces shade alpha to 1.0 when fog is active, so the vertex alpha channel
is free to carry the factor.

### 2. Texture edge (alpha compare) — ✅ implemented, insufficient on its own

`gfx_gx_emit_tev` now sets `GX_SetAlphaCompare(GX_GREATER, 76, …)` plus
`GX_SetZCompLoc(GX_FALSE)` for shaders carrying `opt_texture_edge`, and restores
`GX_ALWAYS` / `GX_TRUE` otherwise.

It did **not** fix the dark polygons on the intro Mario head, so those come from somewhere
else. Do not assume this task closed that defect.


Direct equivalent:
```c
GX_SetAlphaCompare(GX_GREATER, 76, GX_AOP_AND, GX_ALWAYS, 0);  /* 0.3 * 255 ≈ 76 */
GX_SetZCompLoc(GX_FALSE);   /* Z test after the alpha test */
```
`GX_SetZCompLoc(GX_FALSE)` is **essential**: without it Z is written before the alpha reject,
and transparent pixels occlude what is behind them. That is the classic "foliage cuts holes in
the scenery" bug.

Remember to restore `GX_SetAlphaCompare(GX_ALWAYS, …)` and `GX_SetZCompLoc(GX_TRUE)` when the
option is not active, through the STORY-006 state cache.

### Open defect: layered face decals lose the depth test

**The clearest reproduction in the game is the intro Mario head: the pupil renders behind the
white of the eye.** The eyebrows, moustache and sideburns are affected the same way.

The decisive measurement: with `-DGFX_GX_DEBUG_NO_DEPTH` the face is **perfect** — eyebrows,
sclera, blue iris, black pupil, glint, moustache, all correct. So geometry, textures, colours,
combiner and submission order are all right, and painter's order alone produces the intended
image. **Only the depth test is rejecting the later layers.**

Depth separation between a layer and the face beneath it, read with
`-DGFX_GX_DEBUG_DEPTH -DGFX_GX_DEBUG_DEPTH_FINE` (which shows the low bits of `zn`, one unit
≈ 3.8e-6):

| Sample | low bits |
|---|---|
| left pupil / left cheek | 155 / 153 |
| right pupil / right cheek | 123 / 113 |
| nose | 30 |
| eyebrow | 106 |

So Δ`zn` ≈ 1e-5 to 4e-5 — small, but roughly 170 to 670 levels in a 24-bit buffer, far from
the quantisation floor.

Ruled out by measurement, each with one build:

| Hypothesis | Result |
|---|---|
| The per-batch hardware projection collapses the layers | no change with `-DGFX_GX_DEBUG_NO_HWPERSP` |
| Anti-aliasing forces a 16-bit Z buffer | no change with `aa` forced off (kept anyway: 24-bit depth is worth more here than edge AA) |
| Missing ZMODE_DEC bias | no change with a 8e-4 NDC bias — so these layers are not flagged as decals |
| `GX_SetZCompLoc(GX_TRUE)` lets transparent texels stamp depth | no change with it forced to `GX_FALSE` (kept anyway: it is the correct setting for layered cut-outs) |
| Inverted comparison | `GX_GEQUAL` is worse — the background then covers everything |

**Next step, and it must be a measurement rather than another hypothesis:** the samples above
read the *topmost* surface at each pixel, so they cannot separate the pupil's own depth from
the sclera's. Add a debug that renders only a chosen range of draw calls, isolate the eye
layers, and read each one's depth directly. That answers whether the later layer is genuinely
farther — and if it is, the question moves upstream to what `gfx_pc` hands us.

### 3. Z decal

The OpenGL backend uses `glPolygonOffset`. GX has no direct equivalent; options, by preference:
1. `GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE)` — `≤` test with no Z write. Enough in most
   cases and free. **This is what is implemented today.**
2. Manual Z bias on the vertices in `draw_triangles` when `zmode_decal` is active (a small
   constant offset in clip space).
3. `GX_SetZTexture` — overkill for this.

Start with (1), check visually, only move to (2) if z-fighting remains.

### 4. Noise

The OpenGL shader computes a pseudo-random value from the frame number and the screen
position. TEV cannot generate randomness.

Option retained for v1: **a small noise texture** (64×64, generated at boot), bound to
`GX_TEXMAP2`, sampled with coordinates offset each frame via `GX_LoadTexMtxImm`. One TEV stage
combines it into alpha. Cost: a 16 KB texture and one TEV stage, only for the `shader_id`s
carrying `SHADER_OPT_NOISE`.

**Low priority**: `opt_noise` is used by very few SM64 combiners (fades, and the dissolve
effect on some objects). Shipping the story without it and completing later is acceptable —
it is the only one of the four that can be deferred.

## Files touched

- `src/pc/gfx/gfx_gx.c`

## Notes and risks

- These four effects sit **after** the STORY-007 combiner in the TEV chain. Recount the
  stages: combiner (1-2) + fog (1) + noise (1) = 4 at most out of 16. No risk of overflow, but
  `GX_SetNumTevStages` must reflect the exact total.
- Z-fighting on real hardware can differ from Dolphin (Z-buffer precision). Validate decals on
  a console.
- `GX_SetZCompLoc` is global state: leaving it at `GX_FALSE` permanently costs performance
  (loss of early Z rejection). Toggle it properly through the state cache.
