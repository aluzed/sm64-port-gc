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
| The STORY-010 alpha dither | `-DGFX_GX_NOISE=0` drops the dither and its screen-space texgen entirely. The triangles survive it, so the newest change in the tree is not the cause (2026-08-17) |

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

### The trade above was priced wrong — 2026-08-17

Reported from hardware on the build that carried it: **walking at the junction of two map faces
and turning the camera makes polygons appear that should be hidden from where Mario stands.**
The wedge is gone; what replaced it is geometry showing through the ground.

That is the "sorting artefact" the fix accepted, and calling it that understated it. A
constant-depth projection does not make the depth *approximate*, it makes it **constant**: the
whole batch lands on one plane, so a large floor stops occluding what is behind it. And the
batches that reach the fallback are exactly the ones that must occlude — large floor and wall
polygons seen nearly edge-on are precisely the shapes whose `1/w` spread collapses. The trade
bought a clipped wedge and sold the depth buffer, on the geometry least able to afford it.

The error was treating `p` and `q` as a property of the batch. They are a property of
`gfx_pc`'s projection matrix: every batch drawn through the same matrix recovers the same pair,
and a batch fails to recover it only when it is too ill-conditioned to *measure* — never
because its depth is genuinely different.

**Fix: a batch that cannot fit a projection borrows the last one that did.** The pair is
recorded whenever a fit holds, and checked against the borrowing batch's own vertices before
use, so a projection change is noticed rather than assumed away. Constant depth stays, demoted
to what it should always have been: the last resort when nothing has fitted yet, or when the
recorded pair demonstrably belongs to another projection.

The same borrow now covers the residual-failure case, which previously used a fit the residual
check had just declared wrong.

The borrow changed nothing. Reported the same day, under Dolphin: same triangle, same edge,
100% reproducible. So the constant-depth fallback was not the cause either, and this fix is
retained on its own merits — inventing a depth was still wrong — not as a cure.

### The report that moves the search — 2026-08-17

**The triangles appear during the intro.** That is worth more than the edge in the castle
grounds, on two counts. It is seconds from boot with no navigation, so the loop is short. And
the intro draws the Mario head through the Goddard renderer, not level geometry — so whatever
this is, it is not specific to a surface, a level, or a texture.

Two changes were candidates for "it was not there before". One is now excluded by measurement
(the dither, see the ruled-out table). The other is the near-plane commit, and
`-DGFX_GX_DEBUG_OLD_NEARPLANE` reverts it in place — both halves, the batch exemption and the
CPU path's mirroring — to date the regression without a bisect. **Reverted, the triangles are
gone.** So the commit introduced them.

### Closed: the one refusal that was never covered — 2026-08-17

`-DGFX_GX_DEBUG_PROJ_TINT` marks each batch by the route it took through
`gfx_gx_setup_perspective`. **The triangles come out magenta: the CPU divide.** Everything
else keeps its texture.

That is the whole answer. Since the near-plane commit, a batch crossing the near plane goes to
the GP whatever its fit — except for one refusal that was never touched:

```c
if (usable < 3) {
    return false;
}
```

Fewer than three vertices in front of the eye. The commit's reasoning applies to this batch
*more* strongly than to any other — it is the one most behind the eye — and it was the one case
still handed to a path that cannot clip. Worse in combination with the commit's other half:
the CPU path stopped flinging a `w <= 0` vertex out of frame, where it was discarded, and now
collapses it onto `(0, 0)`. The centre of the screen. So the triangle is dragged into the
middle of the image rather than off the edge of it.

Being unable to *measure* a projection is not being unable to *use* one. The batch now borrows
the last fit that held and lets the GP clip, exactly as the two branches below it already do.
With no usable vertex at all the batch is entirely behind the eye and the GP discards it, so
any consistent projection serves.

The collapse is left in place. It is bounded and correct in isolation, and it is now only
reachable through `-DGFX_GX_HW_PERSP=0`.

### Method note, second entry

Three instruments were built before one of them measured anything. The first painted every
batch flat by path, which made the nominal path — almost the whole screen — a single green
field with no landmarks: unusable, and correctly refused by the reporter. The second tinted the
vertex colour, which the TEV is free to ignore, so surfaces went unmarked for two entirely
different reasons and an unmarked surface meant nothing. The third read `dbg_proj_path` before
`gfx_gx_setup_perspective` had set it, so its two halves disagreed about which batch they were
looking at.

Only the third was caught before it produced a false conclusion, and only because it shipped
with `-DGFX_GX_DEBUG_PROJ_TINT_SELFTEST`, which forces every batch onto a marked path so the
screen must come out entirely magenta. It did not: grass came out white. **Calibrate an
instrument before trusting a negative result from it.** A view that cannot fail visibly will
eventually report silence as evidence.

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
