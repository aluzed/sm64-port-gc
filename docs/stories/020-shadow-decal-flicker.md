# STORY-020 — Shadows flicker and drop out

**Epic:** 2 — GX rendering
**Status:** ✅ **Fixed and confirmed on a real GameCube** (2026-08-16). 1e-4 was the right size first time; no shadow was reported detaching from a slope
**Depends on:** STORY-009, STORY-010
**Estimate:** S (half a day)
**Platform:** GC + Wii

## Report

Tree shadows shimmer, and in places the shadow is missing altogether. Seen on a real
GameCube, booted through Swiss. Everything else in the same scene is stable: the picture,
the sound and the frame rate are all fine.

## Likely cause, and it is a known omission

A shadow is a **Z decal**: `gfx_pc` sets `ZMODE_DEC` and the backend must draw it *on* the
surface below without fighting it. The reference backend does that with two things together:

```c
glPolygonOffset(-2, -2);          // pull the decal towards the viewer
glEnable(GL_POLYGON_OFFSET_FILL);
```

and leaves the depth write enabled. Ours does **half** of it:

```c
if (gx_state.zmode_decal) {
    GX_SetZMode(GX_TRUE, GFX_GX_ZFUNC_NEARER, GX_FALSE);   // test, never write
    return;
}
```

No bias. A shadow is coplanar with the ground it sits on, so its interpolated depth lands
either side of the ground's from pixel to pixel and frame to frame: `GX_LEQUAL` passes on some
pixels and fails on others. That is z-fighting, and shimmering plus missing patches is exactly
what it looks like.

There *was* a bias — `GFX_GX_DECAL_BIAS 0.0008f` — added while chasing the layered-face defect
and removed on 2026-08-16 when the renderer was reverted to a known-good state. It was removed
because it did not fix that defect, which is true; it was never the wrong idea for shadows,
which is what it was for.

## The trap to avoid

**The bias has to be applied on both projection paths, or it recreates the two-convention bug
that cost two sessions** (see [STORY-009](009-vertex-format-draw-triangles.md)). A bias
applied only in the CPU divide would make decals sort differently depending on which path
their batch happened to take.

The CPU path is the easy one: subtract the bias from the emitted `z`. The hardware path needs
it folded into the projection, and it lands in one coefficient. With

```
z_ndc = -mt22 + mt23/w
```

a constant NDC bias `b` towards the viewer — more negative — is `mt22 += b`. Nothing else
changes, and the two paths stay derived from the same relation.

The previous attempt sidestepped this by forcing decals onto the CPU path. That works but
costs perspective-correct interpolation on every decal, and it is unnecessary now that the
coefficient is understood.

## What landed

The bias is back, applied on both projection paths from a single constant, `GFX_GX_DECAL_BIAS`,
overridable at build time.

- CPU divide: `z -= bias`, since nearer is more negative in GX's `[-1, 0]`. Clamped at −1, so a
  decal on geometry already at the near plane is not pushed past it and clipped away.
- Hardware projection: `mt22 += bias`. Subtracting `b` from `z_ndc = -mt22 + mt23/w` is exactly
  that, so the two paths stay derived from one relation instead of being reasoned out twice —
  which is the mistake that cost two sessions in [STORY-009](009-vertex-format-draw-triangles.md).

Value: **1e-4**, about sixteen hundred levels of the 24-bit buffer. It only has to break a tie:
a shadow is coplanar with its ground, so the two depths differ by interpolation rounding rather
than by any real distance. At the distance the camera normally sits from the floor that is
under a world unit.

The tension to keep in mind if it needs changing: a constant bias in NDC is a small world-space
offset near the camera and a large one far away, because the depth range compresses with
distance. Too small and the shimmer returns; too large and a shadow lifts off a slope seen
edge-on, or punches through a thin floor.

Dolphin will not settle this. It renders shadows without visible fighting at its own internal
resolution, and the defect was reported on a CRT in the first place.

## Tasks

1. ✅ Reinstate the decal bias, applied on **both** paths as above.
2. ✅ Size confirmed on hardware at 1e-4: the shimmer is gone and nothing detaches. Left as written, with the reasoning, in case a scene is found that needs otherwise.

   Original note: size it by measurement, not by taste. Depth is 24-bit over `zn ∈ [0,1]`, so one level is
   about 6e-8. The bias has to clear the interpolation error across a large ground polygon
   without lifting a shadow visibly off a slope seen edge-on. Start from the previous 8e-4 and
   check both extremes.
3. Check the other decals in the same pass: Mario's own shadow, footprints in snow and sand,
   the painting surfaces, and the metal-grate floors.
4. Only if a constant bias proves insufficient on steep ground, consider a slope-scaled one —
   which is what OpenGL's second `glPolygonOffset` argument does and what GX has no equivalent
   for.

## Acceptance criteria

- [ ] Tree shadows are stable while the camera orbits, at any distance.
- [ ] No shadow is missing.
- [ ] No shadow visibly detaches from the ground on a slope.
- [ ] Sorting between objects is unchanged — verify the intro Mario head still renders its
      pupils in front of the sclera, which is the canary for a depth regression.

## Files touched

- `src/pc/gfx/gfx_gx.c` — `gfx_gx_apply_zmode`, `gfx_gx_setup_perspective`, `draw_triangles`
