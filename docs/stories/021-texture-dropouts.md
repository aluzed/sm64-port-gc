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
