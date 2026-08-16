# STORY-021 — Textures drop in and out with camera movement

**Epic:** 2 — GX rendering
**Status:** To do — reported under Dolphin and confirmed on **real hardware**, 2026-08-16
**Depends on:** STORY-008, STORY-009
**Estimate:** M — the cheap explanations are already gone
**Platform:** GC + Wii

## Report

Some surfaces lose their texture for a moment and get it back, and it tracks camera movement
rather than time. The water surface is the clearest and most repeatable case: it vanishes for
about half a second when the camera passes near the level of the surface. Elsewhere it shows
as a large surface rendering flat and pale instead of textured.

Intermittent, never fatal, and present on both Dolphin and hardware.

## What has been ruled out, each by a measurement

| Hypothesis | How it was ruled out |
|---|---|
| Distance fog washing surfaces out | the defect survives `-DGFX_GX_FOG=0` |
| The texture pool wrapping and aliasing two textures | `gfx_pc`'s cache has 512 entries and calls `new_texture()` once per entry; our pool is 512, so it cannot wrap |
| `memalign` failing on the larger textures | `-DGFX_GX_DEBUG_TEXFAIL` binds a magenta fallback whenever a texture object is invalid. No magenta ever appeared |
| The `GX_TF_RGBA8` swizzle | Dolphin's texture dump shows all 114 uploaded textures correct, including the non-square 64×32, 32×64 and 128×16 where a stride error would show first |
| The previous texture being left bound | this **was** a real bug and is fixed: the load used to be skipped when a texture object was invalid, leaving whatever was bound before. There is an explicit fallback now |

So the textures we upload are correct, they are bound, and their objects are valid.

## Sharpened by hardware: it happens at the screen edge

**"When I stand in certain corners and an edge reaches the very edge of the screen, I get small
texture bugs."** Reported on a real GameCube, reproducible on demand.

That is a much narrower statement than "with camera movement", and it points at **clipping**
rather than at the projection fit alone: the trigger is a polygon being cut by the frustum
boundary, not the camera moving as such. A corner is where a wall polygon is simultaneously
very close, very large in screen terms, and cut on at least one side.

### Cause, on the second reading of the report

Restated by the reporter: **turning the camera until a polygon edge reaches a corner of the
screen.** That is a polygon crossing the **near plane**, and the wedge is not a texture at all
— it is a vertex projected through a negative `w`.

`gfx_pc` does no near-plane clipping. A vertex behind the eye arrives with `w <= 0`, and the
CPU divide mirrors it through the origin and flings it off screen, dragging its triangle into a
corner as a large flat sheet. It looks like a missing texture because a triangle stretched over
a quarter of the screen samples a handful of texels.

The hardware path already handled this: feed view space and the GP clips. The hole was that a
batch **rejected** by the projection fit fell back to the CPU divide — and the fit was made
stricter earlier the same day, so more batches were falling through.

**Fix: a batch that crosses the near plane takes the hardware path whatever its fit looks
like.** Only the GP can cut it correctly, and the alternative is not a slightly worse image but
a wedge across a quarter of the screen. Two cases:

- the fit is good, or merely fails the residual check → use the fitted coefficients anyway;
- there is not enough spread in `1/w` to fit at all → use a constant-depth projection, `q = 0`.
  `x` and `y` stay exact and only depth is approximate, on a batch whose depth barely varies.

An imperfect depth on one batch is a sorting artefact. A CPU divide by a negative `w` is a
quarter of the screen.

The CPU path also stops mirroring: a vertex with `w <= 0` now collapses to the centre of the
near plane rather than being flung. Only reachable when fewer than three vertices are in front
of the eye, where there is nothing to fit — degenerate either way, but bounded.

## The remaining lead

The per-batch projection fit in `gfx_gx_setup_perspective`. A large near-flat quad is its
ill-conditioned case: when the camera comes level with the surface the spread in `1/w`
collapses, and the batch either fails the fit — falling back to the CPU divide, which does not
clip at the near plane — or passes it with a residual large enough to push vertices outside
`[0, 1]`, where GX clips them away.

That fits the symptom precisely: **camera-dependent, brief, and worst on big flat surfaces
seen edge-on**, which is exactly what water is.

The conditioning threshold is the place to start:

```c
if (iw_hi - iw_lo < 0.01f * iw_hi) {
    return false;
}
```

## Suggested approach, in order

1. Add a debug view that colours batches by which projection path they took. If the water
   turns out to change path at the moment it disappears, that closes it.
2. If it does, the question becomes what the CPU fallback should do for a batch that has real
   perspective but too little spread to fit — the current answer, affine interpolation with no
   near-plane clipping, is the worst of both.
3. Only then look further upstream.

## Method note

This defect has already absorbed a lot of time, most of it wasted on hypotheses tested by
looking rather than by measuring. Every entry in the ruled-out table above came from one build
that answered one question. That is the rate to keep.

## Files touched

- `src/pc/gfx/gfx_gx.c`
