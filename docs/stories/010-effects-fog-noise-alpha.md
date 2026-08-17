# STORY-010 — Effects: fog, noise, alpha compare, Z decals

**Epic:** 2 — GX rendering
**Status:** 🟡 All four effects **implemented**; none of the four **validated on screen** — ⬅️ **next**
**Depends on:** STORY-007, STORY-008, STORY-009
**Estimate:** M (2-3 d)
**Platform:** GC + Wii

## Task 0 — ✅ resolved: three faults stacked in the depth path

Inherited from [STORY-009](009-vertex-format-draw-triangles.md) and now fixed. **The whole
scene renders, in the right order**, including the layered face decals that used to draw the
white of Mario's eyes in front of his pupils.

Three independent faults, each of which masked the others:

1. **The mapping.** `gfx_pc.c` does `z = (z + w) / 2` when `z_is_from_0_to_1()` is true, which
   maps `[-1 near, +1 far]` to `[0 near, 1 far]`. GX wants −1 near and 0 far, so the mapping is
   the shift `z/w - 1` with `GX_LEQUAL`. It had been changed to a negation, which inverts the
   sort order. See the correction in [STORY-006](006-gx-backend-skeleton.md): the convention is
   in the source and should be read there, not inferred from a screenshot.

2. **Two conventions in one buffer.** STORY-009's per-batch hardware projection derived depth
   independently of the CPU path. Both now come from the same relation.

3. **The state cache versus the EFB→XFB copy.** `gfx_ogc_copy_to_xfb()` must force
   `GX_SetZMode(GX_TRUE, …, GX_TRUE)` and `GX_SetColorUpdate(GX_TRUE)`, or `GX_CopyDisp` will
   not clear the EFB — and it does so behind `gfx_gx.c`'s state cache, which only emits on a
   change. When the first draw of a frame matched what the cache already believed, nothing was
   emitted and that draw ran with the copy's depth state. SM64's full-screen background
   rectangle asks for no depth test; left writing at `zn = 0`, the near plane, it stamped the
   whole buffer, and only the surfaces that draw without testing — the HUD, PRESS START — came
   through. Whether the first draw matched varied frame to frame, so the scene and the HUD took
   turns. `gfx_gx_start_frame` now re-emits both.

Fault 3 is the general lesson: **any GX state written outside `gfx_gx.c` has to be re-emitted,
because the state cache cannot see it.** The copy is the only such writer today.

The depth mask gating survived all of this and is not a compensating change: OpenGL writes
nothing to the depth buffer when `GL_DEPTH_TEST` is disabled whatever `glDepthMask` says, GX
honours `update_enable` regardless, and `gfx_pc` passes both flags through from the N64 render
mode.

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

### 1. Fog — ✅ implemented, unvalidated

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

**(A) is what is implemented.** The fog colour and factor travel as the RGBA of `GX_VA_CLR1`,
a second rasterised channel enabled only for shaders carrying `opt_fog`, and the final stage
reads them as `RASC` and `RASA`: `out = d + (1-c)*a + c*b` with `d = 0`, `a = CPREV`,
`b = RASC`, `c = RASA` is the mix exactly, with no arithmetic of our own. Build with
`-DGFX_GX_FOG=0` to drop the stage — the A/B to run whenever a scene looks washed out, since
an over-applied fog and a missing texture are hard to tell apart by eye.

### 2. Texture edge (alpha compare) — ✅ implemented

`gfx_gx_emit_tev` now sets `GX_SetAlphaCompare(GX_GREATER, 76, …)` plus
`GX_SetZCompLoc(GX_FALSE)` for shaders carrying `opt_texture_edge`, and restores
`GX_ALWAYS` / `GX_TRUE` otherwise.

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

### Closed: layered face decals — and how not to debug the next one

The intro Mario head used to draw the pupil behind the white of the eye, with the eyebrows,
moustache and sideburns affected the same way. It is fixed by task 0 above; nothing in this
section is outstanding.

It is recorded because the *process* went badly and cost two sessions. Five hypotheses were
raised and each dismissed by a build — the hardware projection, 16-bit Z from anti-aliasing, a
missing `ZMODE_DEC` bias, `GX_SetZCompLoc`, an inverted comparison — and none of them was the
cause. Depth samples were taken to five decimal places from a buffer that was being corrupted
by two other faults at the same time, and they were treated as ground truth.

Three things would have shortened it:

- **Read conventions in the source; measure only behaviour.** The depth mapping was settled in
  the end by three lines of `gfx_pc.c`, not by any screenshot.
- **A measurement is only evidence once the thing measured is known to be sound.** Every depth
  reading taken during this period described a corrupted buffer.
- **When something used to work, bisect.** The state that rendered correctly was still in the
  history the whole time. Correlating a timestamped screenshot against `git log` located it in
  one command, after five builds spent hypothesising forward.

Two changes made while chasing this were kept because they are correct in their own right:
anti-aliasing forced off, for a 24-bit rather than 16-bit depth buffer, and
`GX_SetZCompLoc(GX_FALSE)` for texture-edge shaders. Two were reverted: the `ZMODE_DEC` NDC
bias and forcing decals onto the CPU path, neither of which these layers ever needed.

### 3. Z decal

The OpenGL backend uses `glPolygonOffset`. GX has no direct equivalent; options, by preference:
1. `GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE)` — `≤` test with no Z write. Enough in most
   cases and free. **This is what is implemented today.**
2. Manual Z bias on the vertices in `draw_triangles` when `zmode_decal` is active (a small
   constant offset in clip space).
3. `GX_SetZTexture` — overkill for this.

Start with (1), check visually, only move to (2) if z-fighting remains.

### 4. Noise — ✅ implemented, unvalidated

The OpenGL shader computes a pseudo-random value from the frame number and the screen
position. TEV cannot generate randomness.

Read the reference before translating it, because the name is misleading:

```glsl
texel.a *= floor(random(vec3(floor(gl_FragCoord.xy * (240.0 / window_height)), frame_count)) + 0.5);
```

`floor(random + 0.5)` is **0 or 1**, not a continuous grain. So this is a screen door — half
the cells keep their alpha and half lose it — which is what makes SM64's fades *dissolve*
rather than cross-fade. It applies only when `opt_alpha` is set, and it comes **after** fog in
the chain.

Implemented as planned: a 64×64 `GX_TF_I8` texture of pre-rolled zeroes and ones generated at
boot by an LCG, on `GX_TEXMAP2`, with one final TEV stage doing `APREV * TEXA`.

Three things the plan did not anticipate:

- **The coordinate has to be screen space**, or the pattern swims with the geometry instead of
  staying put on screen. `gfx_pc` supplies no such coordinate, so it comes from a texgen on
  `GX_TG_POS`.
- **The two projection paths submit different spaces**, so one texgen matrix will not do. The
  CPU path submits NDC and wants `q = 1`; the hardware path submits view space with `z = -w`
  and wants `q = -z`, so `GX_TG_MTX3x4` does the divide. The constant term has to ride `z` in
  that second case or it would be divided too and the pattern would shrink with distance. The
  matrix is therefore loaded **per batch**, after the projection is chosen.
- **I8 is tiled in 8×4 blocks**, but this texture needs no swizzle: the content is random, so
  any permutation of it is equally random. Stated in the code because the absence of the
  swizzle every other texture needs looks like an omission.

The 240-line virtual raster is reproduced, so one dither cell is one N64 pixel whatever the
output resolution — the stipple stays the same size at 240p, 480i and 480p. The pattern is
re-offset by a whole number of texels each frame rather than slid, since a smooth shift would
read as the dither crawling. `-DGFX_GX_NOISE=0` drops the stage, which degrades to a smooth
fade rather than an artefact, as this story requires.

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
