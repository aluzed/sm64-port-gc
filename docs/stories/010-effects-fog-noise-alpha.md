# STORY-010 — Effects: fog, noise, alpha compare, Z decals

**Epic:** 2 — GX rendering
**Status:** To do — ⬅️ **next**, and task 0 below now blocks the whole in-game image
**Depends on:** STORY-007, STORY-008, STORY-009
**Estimate:** M (2-3 d)
**Platform:** GC + Wii

## Task 0 — the skybox occludes the level (blocking)

Inherited from [STORY-009](009-vertex-format-draw-triangles.md), whose diagnostics located it
precisely: level geometry is submitted correctly and then **rejected by the depth test**. A
full-screen surface — the skybox — sits at a constant `zn` ≈ 0.33 and writes depth, while level
geometry in a perspective projection sits at `zn` close to 1 and therefore loses `GX_LEQUAL`.
The HUD, at `zn` = 0, is the only thing that survives on top.

Reproduce in one run: `-DGFX_GX_DEBUG_BATCH` shows six large quads and nothing else; adding
`-DGFX_GX_DEBUG_NO_DEPTH` makes the level appear.

First thing to check: **which end of `[0, 1]` the GX viewport maps the near plane to.**
`GX_SetViewport(..., 0.0f, 1.0f)` was assumed to put the near plane at 0, to match `GX_LEQUAL`.
If it is the other way round, every depth relation in the backend is inverted, which would
explain the observation exactly. Then `GX_SetZCompLoc`, then a call-by-call comparison against
`gfx_opengl.c` for one skybox draw.

Note that a related fix already landed: the depth mask is now gated on the depth test, because
OpenGL writes no depth at all when `GL_DEPTH_TEST` is off while GX still honours
`update_enable`. Necessary, but not sufficient.

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

### 2. Texture edge (alpha compare)

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
