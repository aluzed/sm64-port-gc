# STORY-007 — Translating the N64 colour combiner into TEV stages

**Epic:** 2 — GX rendering
**Status:** To do — ⬅️ **next story**, this is what holds the colours
**Depends on:** STORY-006
**Estimate:** XL (5-8 d) — **the technical heart of the project**
**Platform:** GC + Wii

> **Observed under Dolphin after STORY-008**: `gfx_gx_load_shader` currently forces every
> `shader_id` onto `GX_MODULATE` and only reads combiner input 1. The symptom is clear: the
> Mario head from the intro renders with the right shape and the right highlights, but
> **entirely black** — input 1 carries the per-vertex lighting, input 2 the base colour, and
> the latter is ignored. Textures and geometry are correct, as the pixel-perfect HUD proves.

## Context

The OpenGL backend **generates GLSL at runtime**: for each `shader_id`,
`gfx_cc_get_features()` decodes the N64 combiner and
`gfx_opengl_create_and_load_new_shader()` assembles a vertex and a fragment shader as strings.

GX has no shaders. It has a chain of **16 TEV stages**, fixed function. The good news: TEV is
the direct descendant of the N64 RDP combiner, so the translation is natural — far more so
than GLSL. It just has to be done correctly.

### The two formulas

**N64 combiner** (what `gfx_cc` decodes into `c[colorOrAlpha][0..3]` = `a, b, c, d`):

```
out = (a - b) * c + d
```

**TEV stage** (`GX_SetTevColorIn` + `GX_SetTevColorOp`):

```
out = (d ± ((1 - c) * a + c * b)) * scale + bias      [optional clamp]
```

So TEV natively does a **linear interpolation**, not the `(a-b)*c+d` form. The translation
depends on the case, and `gfx_cc` already classifies them via `do_single` / `do_multiply` /
`do_mix`:

| `gfx_cc` case | N64 formula | TEV configuration | Stages |
|---|---|---|---|
| `do_single` | `d` | `a=0, b=0, c=0, d=D` | **1** |
| `do_multiply` | `a * c` | `a=0, b=A, c=C, d=0` → `C·A` | **1** |
| `do_mix` (b == d) | `lerp(b, a, c)` | `a=B, b=A, c=C, d=0` → `(1-C)·B + C·A` | **1** |
| general | `(a-b)*c + d` | stage 1: `a=0, b=A, c=C, d=D` → `D + C·A`<br>stage 2: `a=0, b=B, c=C, d=PREV`, op `GX_TEV_SUB` → `PREV − C·B` | **2** |

Nearly every SM64 combiner falls into the first three cases → **one TEV stage** most of the
time, two at worst, out of 16 available. Comfortable.

> **Trap in the general case:** TEV clamps each stage's output to `[0,1]`. `D + C·A` can
> legitimately exceed 1 before stage 2 subtracts `C·B`. So pass `clamp = GX_FALSE` on stage
> 1's `GX_SetTevColorOp` (TEV registers are signed and wider than 8 bits), and `GX_TRUE` only
> on the last stage.

### The inputs (`SHADER_INPUT_1..4`)

`gfx_pc.c` writes the combiner inputs **per vertex** into `buf_vbo`, resolving
`comb->shader_input_mapping[k][j]` to `rdp.prim_color`, `v_arr[i]->color` (shade),
`rdp.env_color` or a computed LOD value.

Two subtleties not to miss:

1. The mapping **can differ between the colour component (`k=0`) and alpha (`k=1`)**: an
   input's RGB may come from `PRIM` while its alpha comes from `SHADE`.
2. Of the four possible inputs, **only one actually varies per vertex**: `CC_SHADE`. `PRIM`,
   `ENV` and `LOD` are constant for the whole draw.

But GX only has **two per-vertex colour channels** (`GX_VA_CLR0`, `GX_VA_CLR1`) against four
possible inputs. Hence the strategy:

- the input that varies per vertex → rasterised colour channel (`GX_CC_RASC` / `GX_CA_RASA`);
- constant inputs → **TEV registers** (`GX_TEVREG0/1/2` via `GX_SetTevColorS10`) or constant
  colours (`GX_SetTevKColor` + `GX_CC_KONST`).

## Goal

As a developer, I want a function that translates a `shader_id` into a complete TEV
configuration and loads it on demand, so that colours, texture modulation and vertex lighting
match the reference rendering.

## Acceptance criteria

- [ ] `gfx_gx_create_and_load_new_shader(shader_id)` produces a valid TEV configuration for
      **every** `shader_id` SM64 generates (list to extract, see task 1).
- [ ] All four cases above are implemented, colour **and** alpha, with correct intermediate
      clamp handling.
- [ ] `color_alpha_same` is used to avoid duplicating configuration needlessly.
- [ ] `GX_SetNumTevStages()` is called with the exact count; no leftover stage from a previous
      `shader_id` stays active.
- [ ] `load_shader()` is idempotent and short-circuited when the `shader_id` is already loaded.
- [ ] Visual comparison against the PC reference on at least 6 scenes: title screen, castle
      lobby, Bob-omb Battlefield (outdoors/fog), Cool Cool Mountain (snow/transparency), the
      Jolly Roger Bay underwater section, and the painting room (texture + lighting).

## Tasks

1. **Extract the real `shader_id` list.** Instrument the PC build: log every `shader_id`
   passed to `create_and_load_new_shader` over a session covering several levels. That gives
   the finite set to support (typically a few dozen) and doubles as a **test basis**. Do this
   first: it turns an open problem into a finite one.

2. **Write the translator** `gfx_gx_setup_tev(struct ShaderProgram *prg)`:
   - decode via `gfx_cc_get_features()` (already written, reused as is);
   - build a `struct TevStage stages[4]` array (operands, operation, output register, clamp);
   - store the **precomputed** configuration in `ShaderProgram`: computed once per
     `shader_id`, not per draw call.

3. **Operand mapping table**:

   | `SHADER_*` | TEV colour input | TEV alpha input |
   |---|---|---|
   | `SHADER_0` | `GX_CC_ZERO` | `GX_CA_ZERO` |
   | `SHADER_TEXEL0` | `GX_CC_TEXC` | `GX_CA_TEXA` |
   | `SHADER_TEXEL0A` | `GX_CC_TEXA` (alpha→rgb swap) | `GX_CA_TEXA` |
   | `SHADER_TEXEL1` | `GX_CC_TEXC` (2nd texmap) | `GX_CA_TEXA` |
   | `SHADER_INPUT_1..4` | `GX_CC_RASC` or `GX_CC_C0/C1/C2` / `GX_CC_KONST` | same in `GX_CA_*` |

   `SHADER_TEXEL0A` (texture alpha used as colour) is done cleanly with `GX_SetTevSwapMode`
   plus a `GX_TEV_SWAP` table replicating alpha across RGB. That is exactly what the feature
   exists for; do not emulate it with an extra stage.

4. **Assigning inputs.** For v1, a robust approach with no change to `gfx_pc.c`: at
   `draw_triangles` time, compare each input's value across the batch's vertices. A constant
   input is **hoisted** into a TEV register (`GX_SetTevColorS10(GX_TEVREG0, …)`); a varying one
   goes through a rasterised colour channel. Since at most one input varies in practice,
   `GX_VA_CLR0` suffices.

   The faster but more intrusive alternative is to expose `comb->shader_input_mapping` from
   `gfx_pc.c` to know statically which input is `CC_SHADE`. Keep it in reserve if profiling
   (STORY-018) shows the scan is expensive — the rebase cost is not worth it before then.

5. **Colour channels.** `GX_SetNumChans(1)`, `GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, …,
   GX_SRC_VTX, …)`: no hardware lighting, the vertex colour passes straight through. `gfx_pc`
   has already applied the N64 lighting model on the CPU.

6. **TEV order**: `GX_SetTevOrder(stage, texcoord, texmap, colour_channel)`. Stages that use no
   texture must be given `GX_TEXCOORDNULL, GX_TEXMAP_NULL`, or they sample an uninitialised
   texture (random artefacts).

7. **Validation log.** Keep a table in this document: `shader_id` → stage count → visual status
   (OK / to revisit), filled in as testing proceeds. That is the story's traceability
   deliverable.

## Files touched

- `src/pc/gfx/gfx_gx.c`
- `src/pc/gfx/gfx_cc.c` / `gfx_cc.h` (read only — do not modify)
- `src/pc/gfx/gfx_pc.c` (only if the task-4 alternative is chosen)

## Notes and risks

- **This is the riskiest story in the project.** Budget time for visual tuning, not just for
  writing code. Compare systematically against the PC build running side by side on the same
  scene.
- TEV works in **8 bits per component with signed 10-bit intermediate registers**, where GLSL
  works in float. Slight hue differences are expected and acceptable; sharp differences point
  at a translation error.
- `GX_SetTevKColor` only has 4 constant colours and `GX_SetTevColorS10` only 3 registers
  (`GX_TEVREG0/1/2`) — plenty for ≤ 4 inputs, but not to be wasted.
- Do not start with the two-stage general case: implement `do_single`, `do_multiply` and
  `do_mix`, check that 90 % of the screen is right, then handle the rest. Early visual
  feedback is what makes this story tractable.
