#ifdef ENABLE_GX

// GX rendering backend for GameCube and Wii.
//
// gfx_pc.c does all the transform work on the CPU and hands us triangles that
// are already in clip space, so this file is essentially a rasteriser front
// end: state (depth, blend, viewport, scissor), a TEV configuration per
// shader_id, and vertex submission.
//
// Two things to keep in mind while reading:
//
//  * PR/gbi.h cannot be included here -- <ogc/gx.h> declares Vtx and Mtx with
//    incompatible types (docs/stories/003). The two GBI constants the backend
//    needs are redefined below.
//  * The video bring-up (VI, XFB, GX_Init, EFB->XFB copy parameters) lives in
//    gfx_ogc.c, not here: it is tied to the video mode rather than to the
//    renderer, and it must keep working in ENABLE_GFX_DUMMY builds.

#include <ogc/gx.h>
#include <ogc/gu.h>
#include <ogc/cache.h>

#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_cc.h"
#include "gfx_gx.h"
#include "gfx_ogc.h"
#include "gfx_rendering_api.h"

// From PR/gbi.h, which we cannot include (see the header comment). These are
// the only two GBI values the rendering backends use -- gfx_opengl.c refers to
// exactly the same pair.
#define G_TX_MIRROR 0x1
#define G_TX_CLAMP  0x2

#define MAX_SHADER_PROGRAMS 64
#define MAX_TEXTURES        512

// Set to 0 to fall back to vertex colours and ignore textures entirely. Useful
// when something stops drawing: sampling a texture that was never uploaded
// renders every textured surface black, which in SM64 covers most of the screen
// and looks exactly like "nothing draws at all".
#ifndef GFX_GX_TEXTURES_IMPLEMENTED
#define GFX_GX_TEXTURES_IMPLEMENTED 1
#endif

// Per-batch hardware perspective projection.
//
// It was off for a while because it fed the depth buffer a convention the CPU
// path did not share, so batches sorted against each other. The two now agree
// (see gfx_gx_setup_perspective), and it is back on for two reasons beyond
// perspective-correct texture interpolation:
//
// gfx_pc does not clip at the near plane -- gfx_sp_tri1 only rejects triangles
// whose three vertices share a rejection bit -- so a triangle straddling it
// arrives with w <= 0 on some vertices. Dividing that on the CPU produces
// nonsense positions and a nonsense depth, which stamps the buffer and makes
// everything drawn afterwards lose the test. Feeding view space instead lets
// the GP's clipper handle it, which is what it is for.
//
// Set to 0 to fall back to a CPU divide everywhere.
#ifndef GFX_GX_HW_PERSP
#define GFX_GX_HW_PERSP 1
#endif

// Worst case is: one stage to stash texel1, two for the general colour form,
// and the alpha form padded to match. Eight leaves room for STORY-010.
#define MAX_TEV_STAGES 8

// GX has three colour/output registers usable as TEV inputs beside TEVPREV,
// which we keep as the accumulator.
#define NUM_TEV_REGS 3

struct TevStage {
    u8 tex_coord, tex_map;
    u8 col_a, col_b, col_c, col_d;   // GX_CC_*
    u8 alp_a, alp_b, alp_c, alp_d;   // GX_CA_*
    u8 col_op, alp_op;               // GX_TEV_ADD / GX_TEV_SUB
    u8 col_clamp, alp_clamp;
    u8 out_reg;                      // GX_TEVPREV, or a register when stashing
    bool alpha_konst_one;            // force alpha to 1.0 through KONST
};

struct ShaderProgram {
    uint32_t shader_id;
    struct CCFeatures cc;
    uint8_t num_floats;   // per vertex, as laid out by gfx_sp_tri1 in gfx_pc.c
    bool used_textures[2];

    // TEV plan. Rebuilt only when the set of per-vertex inputs changes, which
    // in practice means once: see gfx_gx_build_tev.
    struct TevStage stages[MAX_TEV_STAGES];
    uint8_t num_stages;
    int8_t built_for_varying;   // input index routed to the rasteriser, -1 none
    int8_t input_reg[4];        // TEV register holding each input, -1 = rasterised
    int8_t texel1_reg;          // register holding texel1, -1 = unused

    // Which translation case the colour formula took, for -DGFX_GX_DEBUG_CC.
    uint8_t dbg_case;           // 0 single, 1 multiply, 2 mix, 3 general
    bool dbg_uses_texel0a;
};

static struct ShaderProgram shader_program_pool[MAX_SHADER_PROGRAMS];
static uint8_t shader_program_pool_size;
static struct ShaderProgram *cur_shader;

// Mirror of the GX state, so redundant GX_Set* calls never reach the FIFO.
// Every one of them costs FIFO bandwidth that the GP has to chew through.
static struct {
    bool depth_test;
    bool depth_mask;
    bool zmode_decal;
    bool use_alpha;
    bool initialised;
} gx_state;

// Which comparison lets the nearer surface win. GX_LEQUAL, the canonical libogc
// setup: near maps to 0, far to 1, and the depth buffer is cleared to
// GX_MAX_Z24. See gfx_gx_depth_ndc for the mapping that makes this hold.
//
// -DGFX_GX_DEBUG_ZFLIP swaps it, which is how the orientation was pinned down.
#ifdef GFX_GX_DEBUG_ZFLIP
#define GFX_GX_ZFUNC_NEARER GX_GEQUAL
#else
#define GFX_GX_ZFUNC_NEARER GX_LEQUAL
#endif

static void gfx_gx_apply_zmode(void) {
#ifdef GFX_GX_DEBUG_NO_DEPTH
    // Everything draws, painter's order. Combined with -DGFX_GX_DEBUG_BATCH this
    // separates "geometry is never submitted" from "geometry is submitted and
    // fails the depth test".
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    return;
#endif
    // A decal (Mario's shadow, footprints) must test against the surface it
    // sits on but never write depth, otherwise it fights with it.
    if (gx_state.zmode_decal) {
        GX_SetZMode(GX_TRUE, GFX_GX_ZFUNC_NEARER, GX_FALSE);
        return;
    }

    // The mask is gated on the test, because the two APIs disagree about what
    // "no depth test" means. With GL_DEPTH_TEST disabled OpenGL writes nothing
    // to the depth buffer whatever glDepthMask says; GX honours update_enable
    // regardless and treats the comparison as always passing, so it does write.
    //
    // gfx_pc passes both flags through from the N64 state, and SM64 draws its
    // full-screen rectangles -- background, fades, transitions -- with
    // G_ZBUFFER cleared but Z_UPD often still set. Their zn is 0, the near
    // plane. Taken literally on GX, one of them stamps the nearest possible
    // depth across the whole buffer and everything drawn afterwards loses the
    // test, which is the picture appearing and disappearing.
    //
    // This gating is independent of the depth mapping. It was removed once
    // along with a mapping revert, which brought the flicker straight back.
    const bool test = gx_state.depth_test;
    GX_SetZMode(test ? GX_TRUE : GX_FALSE,
                GFX_GX_ZFUNC_NEARER,
                (test && gx_state.depth_mask) ? GX_TRUE : GX_FALSE);
}

static bool gfx_gx_z_is_from_0_to_1(void) {
    // GX clip space runs from -1 at the near plane to 0 at the far plane, so we
    // ask gfx_pc for a [0,1] depth and shift it at submission time. Returning
    // false here would hand us OpenGL's [-1,1] and halve the usable range.
    return true;
}

// -- shaders (TEV configurations) -------------------------------------------

static void gfx_gx_unload_shader(struct ShaderProgram *old_prg) {
    (void) old_prg;
}

// The N64 combiner computes  out = (a - b) * c + d.
// A TEV stage computes       out = d (+/-) ((1 - c) * a + c * b),
// i.e. a linear interpolation, not the same shape. gfx_cc already classifies
// each formula, which maps onto TEV as follows:
//
//   do_single    out = D              -> TEV(0, 0, 0, D)              1 stage
//   do_multiply  out = A*C            -> TEV(0, A, C, 0)              1 stage
//   do_mix       out = lerp(B, A, C)  -> TEV(B, A, C, 0)              1 stage
//   general      out = (A-B)*C + D    -> TEV(0, A, C, D)   unclamped
//                                       TEV(0, B, C, PREV) subtract   2 stages
//
// Nearly every SM64 combiner lands in the first three cases. The general case
// must leave stage 1 unclamped: D + C*A can legitimately exceed 1 before stage 2
// subtracts C*B, and TEV registers are wide enough to hold the overshoot.

static u8 gfx_gx_reg_colour(int reg) {
    static const u8 tbl[NUM_TEV_REGS] = { GX_CC_C0, GX_CC_C1, GX_CC_C2 };
    return tbl[reg];
}

static u8 gfx_gx_reg_alpha_as_colour(int reg) {
    // GX broadcasts a register's alpha across RGB when used as a colour input.
    static const u8 tbl[NUM_TEV_REGS] = { GX_CC_A0, GX_CC_A1, GX_CC_A2 };
    return tbl[reg];
}

static u8 gfx_gx_reg_alpha(int reg) {
    static const u8 tbl[NUM_TEV_REGS] = { GX_CA_A0, GX_CA_A1, GX_CA_A2 };
    return tbl[reg];
}

static u8 gfx_gx_tev_reg_id(int reg) {
    static const u8 tbl[NUM_TEV_REGS] = { GX_TEVREG0, GX_TEVREG1, GX_TEVREG2 };
    return tbl[reg];
}

// SHADER_* operand -> GX colour input.
static u8 gfx_gx_item_to_colour(const struct ShaderProgram *prg, uint8_t item) {
#if !GFX_GX_TEXTURES_IMPLEMENTED
    // Diagnostic build: no texture is ever loaded, so sampling would return
    // black over most of the screen. Neutral white keeps the geometry readable.
    if (item == SHADER_TEXEL0 || item == SHADER_TEXEL0A || item == SHADER_TEXEL1) {
        return GX_CC_ONE;
    }
#endif
    switch (item) {
        case SHADER_0:       return GX_CC_ZERO;
        case SHADER_TEXEL0:  return GX_CC_TEXC;
        // Texture alpha used as a colour. GX_CC_TEXA already replicates alpha
        // across RGB, so no swap table is needed.
        case SHADER_TEXEL0A: return GX_CC_TEXA;
        case SHADER_TEXEL1:
            return prg->texel1_reg >= 0 ? gfx_gx_reg_colour(prg->texel1_reg) : GX_CC_TEXC;
        default: {
            const int idx = item - SHADER_INPUT_1;
            if (idx < 0 || idx > 3) return GX_CC_ZERO;
            const int reg = prg->input_reg[idx];
            return reg < 0 ? GX_CC_RASC : gfx_gx_reg_colour(reg);
        }
    }
}

// SHADER_* operand -> GX alpha input.
static u8 gfx_gx_item_to_alpha(const struct ShaderProgram *prg, uint8_t item) {
    switch (item) {
        case SHADER_0:       return GX_CA_ZERO;
        case SHADER_TEXEL0:
        case SHADER_TEXEL0A: return GX_CA_TEXA;
        case SHADER_TEXEL1:
            return prg->texel1_reg >= 0 ? gfx_gx_reg_alpha(prg->texel1_reg) : GX_CA_TEXA;
        default: {
            const int idx = item - SHADER_INPUT_1;
            if (idx < 0 || idx > 3) return GX_CA_ZERO;
            const int reg = prg->input_reg[idx];
            return reg < 0 ? GX_CA_RASA : gfx_gx_reg_alpha(reg);
        }
    }
}

// One component's (colour or alpha) formula, expressed as up to two TEV stages
// of operand indices into cc->c[comp][].
struct TevForm {
    uint8_t a[2], b[2], c[2], d[2];  // per sub-stage, SHADER_* items
    bool use_prev_d[2];              // stage's d operand is the previous result
    u8 op[2];
    bool clamp[2];
    int count;
};

static void gfx_gx_plan_form(const struct CCFeatures *cc, int comp, struct TevForm *f) {
    const uint8_t *v = cc->c[comp];
    const uint8_t A = v[0], B = v[1], C = v[2], D = v[3];

    f->op[0] = f->op[1] = GX_TEV_ADD;
    f->clamp[0] = f->clamp[1] = true;
    f->use_prev_d[0] = f->use_prev_d[1] = false;

    if (cc->do_single[comp]) {
        f->a[0] = SHADER_0; f->b[0] = SHADER_0; f->c[0] = SHADER_0; f->d[0] = D;
        f->count = 1;
    } else if (cc->do_multiply[comp]) {
        f->a[0] = SHADER_0; f->b[0] = A; f->c[0] = C; f->d[0] = SHADER_0;
        f->count = 1;
    } else if (cc->do_mix[comp]) {
        f->a[0] = B; f->b[0] = A; f->c[0] = C; f->d[0] = SHADER_0;
        f->count = 1;
    } else {
        f->a[0] = SHADER_0; f->b[0] = A; f->c[0] = C; f->d[0] = D;
        f->clamp[0] = false;            // D + C*A may exceed 1 legitimately
        f->a[1] = SHADER_0; f->b[1] = B; f->c[1] = C; f->d[1] = SHADER_0;
        f->use_prev_d[1] = true;
        f->op[1] = GX_TEV_SUB;
        f->count = 2;
    }
}

// Builds the whole TEV plan for a shader, given which combiner input (if any)
// is fed per vertex through the rasteriser.
static void gfx_gx_build_tev(struct ShaderProgram *prg, int varying_input) {
    const struct CCFeatures *cc = &prg->cc;
    const bool use_tex0 = cc->used_textures[0] && GFX_GX_TEXTURES_IMPLEMENTED;
    const bool use_tex1 = cc->used_textures[1] && GFX_GX_TEXTURES_IMPLEMENTED;

    prg->built_for_varying = (int8_t) varying_input;
    prg->texel1_reg = -1;

    // Assign a TEV register to every input except the one that varies per
    // vertex. Only CC_SHADE actually varies in SM64, so one rasterised channel
    // is enough.
    int next_reg = 0;
    for (int i = 0; i < 4; i++) {
        if (i >= cc->num_inputs) {
            prg->input_reg[i] = -1;
        } else if (i == varying_input) {
            prg->input_reg[i] = -1;
        } else if (next_reg < NUM_TEV_REGS) {
            prg->input_reg[i] = (int8_t) next_reg++;
        } else {
            // More constants than registers. Not reachable with SM64's
            // combiners; fall back to the rasterised colour rather than
            // indexing past the register file.
            prg->input_reg[i] = -1;
        }
    }

    int stage = 0;

    // Texel1 has no dedicated texture coordinate set (gfx_pc only ever emits
    // one) and a TEV stage can sample a single texmap, so stash it in a
    // register first and refer to that from the real stages.
    if (use_tex1 && next_reg < NUM_TEV_REGS) {
        prg->texel1_reg = (int8_t) next_reg++;
        struct TevStage *s = &prg->stages[stage++];
        s->tex_coord = GX_TEXCOORD0;
        s->tex_map = GX_TEXMAP1;
        s->col_a = GX_CC_ZERO; s->col_b = GX_CC_ZERO; s->col_c = GX_CC_ZERO; s->col_d = GX_CC_TEXC;
        s->alp_a = GX_CA_ZERO; s->alp_b = GX_CA_ZERO; s->alp_c = GX_CA_ZERO; s->alp_d = GX_CA_TEXA;
        s->col_op = s->alp_op = GX_TEV_ADD;
        s->col_clamp = s->alp_clamp = GX_TRUE;
        s->out_reg = gfx_gx_tev_reg_id(prg->texel1_reg);
        s->alpha_konst_one = false;
    }

    struct TevForm col, alp;
    gfx_gx_plan_form(cc, 0, &col);
    gfx_gx_plan_form(cc, 1, &alp);

    prg->dbg_case = cc->do_single[0] ? 0 : (cc->do_multiply[0] ? 1 : (cc->do_mix[0] ? 2 : 3));
    prg->dbg_uses_texel0a = false;
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 4; j++) {
            if (cc->c[i][j] == SHADER_TEXEL0A) {
                prg->dbg_uses_texel0a = true;
            }
        }
    }

    // Colour and alpha share the same stages, so the shorter form is padded
    // with a pass-through of the previous result.
    const int n = (col.count > alp.count) ? col.count : alp.count;
    const int first = stage;

    for (int i = 0; i < n && stage < MAX_TEV_STAGES; i++, stage++) {
        struct TevStage *s = &prg->stages[stage];

        s->tex_coord = use_tex0 ? GX_TEXCOORD0 : GX_TEXCOORDNULL;
        s->tex_map = use_tex0 ? GX_TEXMAP0 : GX_TEXMAP_NULL;
        s->out_reg = GX_TEVPREV;
        s->alpha_konst_one = false;

        if (i < col.count) {
            s->col_a = gfx_gx_item_to_colour(prg, col.a[i]);
            s->col_b = gfx_gx_item_to_colour(prg, col.b[i]);
            s->col_c = gfx_gx_item_to_colour(prg, col.c[i]);
            s->col_d = col.use_prev_d[i] ? GX_CC_CPREV : gfx_gx_item_to_colour(prg, col.d[i]);
            s->col_op = col.op[i];
            s->col_clamp = col.clamp[i] ? GX_TRUE : GX_FALSE;
        } else {
            s->col_a = GX_CC_ZERO; s->col_b = GX_CC_ZERO; s->col_c = GX_CC_ZERO;
            s->col_d = (stage == first) ? GX_CC_ZERO : GX_CC_CPREV;
            s->col_op = GX_TEV_ADD;
            s->col_clamp = GX_TRUE;
        }

        // In the no-texture diagnostic build the alpha chain would read a
        // texture that does not exist; pin it to 1.0 like the no-alpha case.
        if (!cc->opt_alpha || !GFX_GX_TEXTURES_IMPLEMENTED) {
            // The GLSL backend emits alpha = 1.0 when the combiner has no alpha
            // channel. GX alpha inputs have no ONE, so borrow KONST, whose
            // selector can be pinned to 1.0.
            s->alp_a = GX_CA_ZERO; s->alp_b = GX_CA_ZERO; s->alp_c = GX_CA_ZERO;
            s->alp_d = GX_CA_KONST;
            s->alp_op = GX_TEV_ADD;
            s->alp_clamp = GX_TRUE;
            s->alpha_konst_one = true;
        } else if (i < alp.count) {
            s->alp_a = gfx_gx_item_to_alpha(prg, alp.a[i]);
            s->alp_b = gfx_gx_item_to_alpha(prg, alp.b[i]);
            s->alp_c = gfx_gx_item_to_alpha(prg, alp.c[i]);
            s->alp_d = alp.use_prev_d[i] ? GX_CA_APREV : gfx_gx_item_to_alpha(prg, alp.d[i]);
            s->alp_op = alp.op[i];
            s->alp_clamp = alp.clamp[i] ? GX_TRUE : GX_FALSE;
        } else {
            s->alp_a = GX_CA_ZERO; s->alp_b = GX_CA_ZERO; s->alp_c = GX_CA_ZERO;
            s->alp_d = (stage == first) ? GX_CA_ZERO : GX_CA_APREV;
            s->alp_op = GX_TEV_ADD;
            s->alp_clamp = GX_TRUE;
        }
    }

    prg->num_stages = (uint8_t) stage;
}

static void gfx_gx_emit_tev(const struct ShaderProgram *prg) {
#if defined(GFX_GX_DEBUG_UV) || defined(GFX_GX_DEBUG_BATCH) || defined(GFX_GX_DEBUG_DEPTH) \
    || defined(GFX_GX_DEBUG_ZSTATE) || defined(GFX_GX_DEBUG_INPUTS) \
    || defined(GFX_GX_DEBUG_CC)
    // Flat view: one PASSCLR stage so the vertex colour reaches the screen
    // untouched. draw_triangles feeds it frac(u), frac(v), which turns texture
    // coordinates into a picture -- smooth ramps mean sane coordinates, flat
    // colour means they are not varying.
    (void) prg;
    GX_SetNumTexGens(0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetNumTevStages(1);
    return;
#else
    const bool use_tex = (prg->cc.used_textures[0] || prg->cc.used_textures[1])
                         && GFX_GX_TEXTURES_IMPLEMENTED;

    if (use_tex) {
        GX_SetNumTexGens(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    } else {
        GX_SetNumTexGens(0);
    }

    for (int i = 0; i < prg->num_stages; i++) {
        const struct TevStage *s = &prg->stages[i];
        const u8 st = GX_TEVSTAGE0 + i;

        // Stages that use no texture must be given the null coordinate and null
        // map, or they sample whatever is left in texture memory.
        GX_SetTevOrder(st, s->tex_coord, s->tex_map, GX_COLOR0A0);

        GX_SetTevColorIn(st, s->col_a, s->col_b, s->col_c, s->col_d);
        GX_SetTevColorOp(st, s->col_op, GX_TB_ZERO, GX_CS_SCALE_1, s->col_clamp, s->out_reg);

        GX_SetTevAlphaIn(st, s->alp_a, s->alp_b, s->alp_c, s->alp_d);
        GX_SetTevAlphaOp(st, s->alp_op, GX_TB_ZERO, GX_CS_SCALE_1, s->alp_clamp, s->out_reg);

        if (s->alpha_konst_one) {
            GX_SetTevKAlphaSel(st, GX_TEV_KASEL_1);
        }
    }

    // Cutout textures (grates, foliage, Mario's eyes and cap logo) rely on the
    // alpha test, not on blending: their quads carry fully transparent texels
    // that must be discarded, not blended. Without this every such sprite draws
    // its whole quad, background included.
    //
    // GX_SetZCompLoc(GX_FALSE) moves the Z test after the alpha test. With the
    // default order Z is written before the alpha reject, so discarded texels
    // still occlude whatever is behind them.
    if (prg->cc.opt_texture_edge) {
        GX_SetAlphaCompare(GX_GREATER, 76, GX_AOP_AND, GX_ALWAYS, 0);  // 0.3 * 255
        GX_SetZCompLoc(GX_FALSE);
    } else {
        GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GX_SetZCompLoc(GX_TRUE);
    }

#ifdef GFX_GX_DEBUG_ALPHA
    // Shows the combiner's alpha output as greyscale, by appending one stage
    // that broadcasts the accumulated alpha into RGB. Unlike the other debug
    // views this keeps the real TEV chain, so it reports what the blender and
    // the alpha test actually receive.
    {
        const u8 st = GX_TEVSTAGE0 + prg->num_stages;
        GX_SetTevOrder(st, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevColorIn(st, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_APREV);
        GX_SetTevColorOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaIn(st, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
        GX_SetTevAlphaOp(st, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevKAlphaSel(st, GX_TEV_KASEL_1);
        GX_SetNumTevStages(prg->num_stages + 1);
    }
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    return;
#endif

    GX_SetNumTevStages(prg->num_stages ? prg->num_stages : 1);
#endif
}

static void gfx_gx_load_shader(struct ShaderProgram *new_prg) {
    if (new_prg == cur_shader) {
        return;
    }
    cur_shader = new_prg;
    gfx_gx_emit_tev(new_prg);
}

static struct ShaderProgram *gfx_gx_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures cc;
    gfx_cc_get_features(shader_id, &cc);

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];
    prg->shader_id = shader_id;
    prg->cc = cc;
    prg->used_textures[0] = cc.used_textures[0];
    prg->used_textures[1] = cc.used_textures[1];

    // Mirrors the layout gfx_sp_tri1 writes into buf_vbo: position, then
    // optional texture coordinates, then optional fog, then one block per
    // combiner input.
    prg->num_floats = 4
                    + ((cc.used_textures[0] || cc.used_textures[1]) ? 2 : 0)
                    + (cc.opt_fog ? 4 : 0)
                    + cc.num_inputs * (cc.opt_alpha ? 4 : 3);

    // Input 1 is the per-vertex one for almost every SM64 combiner; the first
    // draw call corrects this if it turns out otherwise.
    gfx_gx_build_tev(prg, cc.num_inputs > 0 ? 0 : -1);

    cur_shader = NULL;   // force the emit below
    gfx_gx_load_shader(prg);
    return prg;
}

static struct ShaderProgram *gfx_gx_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == shader_id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_gx_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->cc.num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

// -- textures ----------------------------------------------------------------

struct GXTexture {
    GXTexObj obj;
    void *data;
    uint32_t data_size;
    uint16_t width, height;
    uint8_t wrap_s, wrap_t;
    uint8_t min_filt, mag_filt;
    bool has_data;
    bool obj_valid;
};

static struct GXTexture texture_pool[MAX_TEXTURES];
static uint32_t texture_pool_size;
// gfx_pc selects a tile, then uploads into whatever it selected last, so the
// upload has to remember which one that was.
static uint32_t cur_tex_id[2];
static int last_selected_tile;

static uint8_t gfx_gx_cm_to_gx(uint32_t val) {
    // Same precedence as gfx_opengl.c: clamp wins over mirror.
    if (val & G_TX_CLAMP) {
        return GX_CLAMP;
    }
    return (val & G_TX_MIRROR) ? GX_MIRROR : GX_REPEAT;
}

// Linear RGBA8888 -> GX_TF_RGBA8, which stores 4x4 texel tiles of 64 bytes:
// 32 bytes of AR pairs followed by 32 bytes of GB pairs. Getting the channel
// order wrong here shows up as an image with permuted colours.
static void gfx_gx_swizzle_rgba8(const uint8_t *src, int width, int height, uint8_t *dst) {
    const int tiles_x = (width + 3) / 4;
    const int tiles_y = (height + 3) / 4;

    for (int by = 0; by < tiles_y; by++) {
        for (int bx = 0; bx < tiles_x; bx++) {
            uint8_t *tile = dst + (((size_t) by * tiles_x) + bx) * 64;
            for (int ty = 0; ty < 4; ty++) {
                // Replicate the edge rather than padding with black: a black
                // border bleeds into the image under bilinear filtering.
                int sy = by * 4 + ty;
                if (sy >= height) sy = height - 1;
                for (int tx = 0; tx < 4; tx++) {
                    int sx = bx * 4 + tx;
                    if (sx >= width) sx = width - 1;

                    const uint8_t *p = src + ((size_t) sy * width + sx) * 4;
                    const int k = (ty * 4 + tx) * 2;
                    tile[k]              = p[3]; // A
                    tile[k + 1]          = p[0]; // R
                    tile[32 + k]         = p[1]; // G
                    tile[32 + k + 1]     = p[2]; // B
                }
            }
        }
    }
}

static void gfx_gx_tex_refresh_obj(struct GXTexture *t) {
    if (!t->has_data) {
        return;
    }
    GX_InitTexObj(&t->obj, t->data, t->width, t->height, GX_TF_RGBA8,
                  t->wrap_s, t->wrap_t, GX_FALSE);
    GX_InitTexObjFilterMode(&t->obj, t->min_filt, t->mag_filt);
    t->obj_valid = true;
}

static uint32_t gfx_gx_new_texture(void) {
    if (texture_pool_size == MAX_TEXTURES) {
        // gfx_pc only ever calls this once per slot of its own 512-entry cache,
        // so this should not be reachable; wrapping round is safer than
        // running off the end of the pool.
        texture_pool_size = 0;
    }
    return texture_pool_size++;
}

static void gfx_gx_select_texture(int tile, uint32_t texture_id) {
    cur_tex_id[tile] = texture_id;
    last_selected_tile = tile;
}

static void gfx_gx_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    struct GXTexture *t = &texture_pool[cur_tex_id[last_selected_tile]];

    const uint32_t size = (uint32_t) (((width + 3) / 4) * ((height + 3) / 4)) * 64;
    if (t->data == NULL || t->data_size != size) {
        free(t->data);
        // 32-byte alignment is required: the GP reads this by DMA.
        t->data = memalign(32, size);
        t->data_size = size;
        if (t->data == NULL) {
            t->has_data = false;
            t->obj_valid = false;
            return;
        }
    }

    gfx_gx_swizzle_rgba8(rgba32_buf, width, height, t->data);
    // Without this the GP reads stale memory while the real bytes sit in the
    // CPU's L1. Dolphin does not care; hardware very much does.
    DCFlushRange(t->data, size);

    t->width = (uint16_t) width;
    t->height = (uint16_t) height;
    t->has_data = true;
    gfx_gx_tex_refresh_obj(t);
}

static void gfx_gx_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    struct GXTexture *t = &texture_pool[cur_tex_id[tile]];

    t->wrap_s = gfx_gx_cm_to_gx(cms);
    t->wrap_t = gfx_gx_cm_to_gx(cmt);
    t->min_filt = linear_filter ? GX_LINEAR : GX_NEAR;
    t->mag_filt = t->min_filt;

    // gfx_pc sets the sampler before the first upload, so there may be nothing
    // to describe yet; the upload will rebuild the object.
    gfx_gx_tex_refresh_obj(t);
}

// -- render state ------------------------------------------------------------

static void gfx_gx_set_depth_test(bool depth_test) {
    if (gx_state.depth_test != depth_test) {
        gx_state.depth_test = depth_test;
        gfx_gx_apply_zmode();
    }
}

static void gfx_gx_set_depth_mask(bool z_upd) {
    if (gx_state.depth_mask != z_upd) {
        gx_state.depth_mask = z_upd;
        gfx_gx_apply_zmode();
    }
}

static void gfx_gx_set_zmode_decal(bool zmode_decal) {
    if (gx_state.zmode_decal != zmode_decal) {
        gx_state.zmode_decal = zmode_decal;
        gfx_gx_apply_zmode();
    }
}

static void gfx_gx_set_viewport(int x, int y, int width, int height) {
    // gfx_pc works in OpenGL conventions, with the origin at the bottom left.
    // GX measures from the top left. Getting this wrong flips the image or
    // clips the wrong edge, so the conversion lives here and nowhere else.
    const int fb_height = (int) gfx_ogc_framebuffer_height();
    GX_SetViewport((f32) x, (f32) (fb_height - y - height),
                   (f32) width, (f32) height, 0.0f, 1.0f);
}

static void gfx_gx_set_scissor(int x, int y, int width, int height) {
    const int fb_height = (int) gfx_ogc_framebuffer_height();
    GX_SetScissor((u32) x, (u32) (fb_height - y - height), (u32) width, (u32) height);
}

static void gfx_gx_set_use_alpha(bool use_alpha) {
    if (gx_state.use_alpha == use_alpha && gx_state.initialised) {
        return;
    }
    gx_state.use_alpha = use_alpha;
    if (use_alpha) {
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    } else {
        GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    }
}

// -- geometry ----------------------------------------------------------------

// -- projection --------------------------------------------------------------
//
// gfx_pc hands us clip space, so the only question is who performs the
// perspective divide.
//
// Doing it on the CPU and feeding an orthographic projection is simple and gets
// depth exactly right, but it leaves GX interpolating texture coordinates
// affinely: w is then constant as far as the hardware is concerned. That is
// invisible on 2D elements and very visible on large floor triangles.
//
// Letting GX do the divide fixes the interpolation, but its perspective
// projection matrix has its last row pinned to (0, 0, -1, 0), so
// w_clip = -z_view and z_ndc = -mt22 + mt23/w depends on w alone. There is no
// way to carry both the game's w and its depth.
//
// The way out is that they are not independent: for a standard perspective
// projection z/w is an affine function of 1/w. So we recover that relation from
// the batch itself -- two vertices give the two coefficients -- and fold it into
// mt22/mt23. Depth then matches the CPU path exactly, with no near/far guesswork
// and no risk of clipping geometry the game considered visible.
//
// A third vertex checks the fit. If it does not hold (a projection that is not a
// standard perspective), we fall back to the CPU divide rather than render
// nonsense.

enum ProjMode { PROJ_NONE, PROJ_ORTHO, PROJ_PERSP };

static enum ProjMode cur_proj_mode;
#if GFX_GX_HW_PERSP
static float cur_proj_mt22, cur_proj_mt23;
#endif

static void gfx_gx_load_ortho(void) {
    if (cur_proj_mode == PROJ_ORTHO) {
        return;
    }
    Mtx44 proj;
    memset(proj, 0, sizeof(proj));
    proj[0][0] = proj[1][1] = proj[2][2] = proj[3][3] = 1.0f;
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    cur_proj_mode = PROJ_ORTHO;
}

#if GFX_GX_HW_PERSP
static void gfx_gx_load_persp(float mt22, float mt23) {
    if (cur_proj_mode == PROJ_PERSP && mt22 == cur_proj_mt22 && mt23 == cur_proj_mt23) {
        return;
    }
    Mtx44 proj;
    memset(proj, 0, sizeof(proj));
    proj[0][0] = 1.0f;   // x_clip = x_view
    proj[1][1] = 1.0f;   // y_clip = y_view
    proj[2][2] = mt22;
    proj[2][3] = mt23;
    proj[3][2] = -1.0f;  // w_clip = -z_view
    GX_LoadProjectionMtx(proj, GX_PERSPECTIVE);
    cur_proj_mode = PROJ_PERSP;
    cur_proj_mt22 = mt22;
    cur_proj_mt23 = mt23;
}

// Returns true and sets up a perspective projection when the batch has varying
// w and its depth fits the expected relation; false means "divide on the CPU".
static bool gfx_gx_setup_perspective(const float *buf, size_t stride, size_t nverts) {
#ifdef GFX_GX_DEBUG_NO_HWPERSP
    (void) buf; (void) stride; (void) nverts;
    return false;   // always divide on the CPU
#endif
    if (nverts < 3) {
        return false;
    }

    size_t lo = 0, hi = 0;
    float iw_lo = 1e30f, iw_hi = -1e30f;
    size_t usable = 0;

    // Vertices at or behind the eye are exactly the ones the hardware path
    // exists for -- dividing them on the CPU is what produces the giant
    // triangles. They cannot contribute to the fit, but their presence must not
    // disqualify the batch.
    for (size_t i = 0; i < nverts; i++) {
        const float w = buf[i * stride + 3];
        if (w <= 1e-6f) {
            continue;
        }
        usable++;
        const float iw = 1.0f / w;
        if (iw < iw_lo) { iw_lo = iw; lo = i; }
        if (iw > iw_hi) { iw_hi = iw; hi = i; }
    }

    if (usable < 3) {
        return false;
    }

    // Not enough spread in 1/w to fit anything meaningful. That covers 2D
    // batches, but also near-flat 3D ones -- and for those the CPU path is not a
    // compromise: when w barely varies, affine interpolation is very nearly
    // correct anyway, while the fit would be ill-conditioned.
    //
    // The threshold is relative. An absolute one lets through batches whose
    // depth spread is a rounding error, and the resulting garbage projection
    // wrecks the depth ordering *within* the object: the intro Mario head lost
    // its eyes and moustache to exactly that.
    if (iw_hi - iw_lo < 0.01f * iw_hi) {
        return false;
    }

    const float zn_lo = buf[lo * stride + 2] * iw_lo;
    const float zn_hi = buf[hi * stride + 2] * iw_hi;

    const float q = (zn_hi - zn_lo) / (iw_hi - iw_lo);
    const float p = zn_lo - q * iw_lo;

    // Validate on the vertex furthest from both anchors.
    size_t mid = lo;
    float best = -1.0f;
    for (size_t i = 0; i < nverts; i++) {
        const float w = buf[i * stride + 3];
        if (w <= 1e-6f) {
            continue;
        }
        const float iw = 1.0f / w;
        const float d = fminf(fabsf(iw - iw_lo), fabsf(iw - iw_hi));
        if (d > best) { best = d; mid = i; }
    }
    const float iw_m = 1.0f / buf[mid * stride + 3];
    const float zn_m = buf[mid * stride + 2] * iw_m;

    // The residual has to be judged against the batch's own depth spread, not
    // against an absolute epsilon. A head occupies about 2% of the depth range,
    // so a 1e-3 absolute tolerance accepts a fit that is 5% wrong across the
    // object -- enough to scramble which of its own polygons is in front.
    const float zn_span = fabsf(zn_hi - zn_lo);
    const float tol = fmaxf(0.02f * zn_span, 1e-6f);
    if (fabsf((p + q * iw_m) - zn_m) > tol) {
        return false;
    }

    // GX gives z_ndc = -mt22 + mt23/w, and the CPU path writes z_ndc = zn - 1
    // with zn = p + q/w. Matching the two: mt22 = 1 - p, mt23 = q.
    //
    // Getting this pair wrong is what split the depth buffer between two
    // conventions; keep it derived from whatever the CPU path in
    // draw_triangles writes, never from the two independently.
    gfx_gx_load_persp(1.0f - p, q);
    return true;
}
#endif  // GFX_GX_HW_PERSP

static u8 float_to_u8(float v) {
    int i = (int) (v * 255.0f + 0.5f);
    if (i < 0) return 0;
    if (i > 255) return 255;
    return (u8) i;
}

static void gfx_gx_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    (void) buf_vbo_len;

    if (cur_shader == NULL) {
        return;
    }

    const struct CCFeatures *cc = &cur_shader->cc;
    const size_t stride = cur_shader->num_floats;
    // gfx_pc decides the buffer layout, so the offsets always follow the
    // combiner; whether we actually submit the coordinates depends on the
    // texture backend being there.
    const bool tex_in_buffer = cc->used_textures[0] || cc->used_textures[1];
    const bool submit_tex = tex_in_buffer && GFX_GX_TEXTURES_IMPLEMENTED;
    const size_t tex_off = 4;
    const size_t input_off = 4 + (tex_in_buffer ? 2 : 0) + (cc->opt_fog ? 4 : 0);
    const bool has_input = cc->num_inputs > 0;
    (void) has_input;   // unused in the debug views
#ifdef GFX_GX_DEBUG_BATCH
    static uint32_t dbg_batch_id;
    dbg_batch_id++;
#endif
    const size_t input_size = cc->opt_alpha ? 4 : 3;
    const size_t num_verts = buf_vbo_num_tris * 3;

    // Which combiner input actually varies across the batch? Only CC_SHADE does
    // in practice; PRIM, ENV and LOD are constant for the whole draw. The
    // varying one goes through the rasteriser, the constants into TEV registers,
    // because GX only has two per-vertex colour channels against four inputs.
    int varying = -1;
    int num_varying = 0;
    for (int j = 0; j < cc->num_inputs; j++) {
        const float *ref = buf_vbo + input_off + j * input_size;
        for (size_t i = 1; i < num_verts; i++) {
            const float *cur = buf_vbo + i * stride + input_off + j * input_size;
            if (memcmp(ref, cur, input_size * sizeof(float)) != 0) {
                if (varying < 0) {
                    varying = j;
                }
                num_varying++;
                break;
            }
        }
    }
#ifdef GFX_GX_DEBUG_INPUTS
    const int dbg_num_varying = num_varying;
#else
    (void) num_varying;
#endif

    if (cur_shader->built_for_varying != varying) {
        gfx_gx_build_tev(cur_shader, varying);
        gfx_gx_emit_tev(cur_shader);
    }

    // Constant inputs are read once, from the first vertex.
    for (int j = 0; j < cc->num_inputs; j++) {
        const int reg = cur_shader->input_reg[j];
        if (reg < 0) {
            continue;
        }
        const float *c = buf_vbo + input_off + j * input_size;
        GXColor col = { float_to_u8(c[0]), float_to_u8(c[1]), float_to_u8(c[2]),
                        cc->opt_alpha ? float_to_u8(c[3]) : 255 };
        GX_SetTevColor(gfx_gx_tev_reg_id(reg), col);
    }

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    if (submit_tex) {
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    }

    if (cc->used_textures[0] && GFX_GX_TEXTURES_IMPLEMENTED) {
        struct GXTexture *t0 = &texture_pool[cur_tex_id[0]];
        if (t0->obj_valid) {
            GX_LoadTexObj(&t0->obj, GX_TEXMAP0);
        }
    }
    if (cc->used_textures[1] && GFX_GX_TEXTURES_IMPLEMENTED) {
        struct GXTexture *t1 = &texture_pool[cur_tex_id[1]];
        if (t1->obj_valid) {
            GX_LoadTexObj(&t1->obj, GX_TEXMAP1);
        }
    }

    // CPU divide, always, with an identity orthographic projection downstream.
    //
    // The per-batch hardware projection is disabled: it derives depth from w
    // through fitted coefficients, on a convention that did not match the one
    // the CPU path uses, so the two paths sorted against each other. Every
    // subsequent attempt to fix the sort order -- flipping the comparison,
    // renegotiating the mapping, gating the depth mask -- was compensating for
    // that split rather than closing it. Build with -DGFX_GX_HW_PERSP=1 to put
    // it back; STORY-009 owns reinstating it with a matching convention, which
    // is what perspective-correct texture interpolation needs.
#if GFX_GX_HW_PERSP
    const bool hw_persp = gfx_gx_setup_perspective(buf_vbo, stride, num_verts);
#else
    const bool hw_persp = false;
#endif
    if (!hw_persp) {
        gfx_gx_load_ortho();
    }

    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, (u16) (buf_vbo_num_tris * 3));
    for (size_t i = 0; i < buf_vbo_num_tris * 3; i++) {
        const float *v = buf_vbo + i * stride;

        if (hw_persp) {
            // Feed view space: the projection turns z_view = -w back into
            // w_clip = w, so the GP divides and interpolates texture
            // coordinates with perspective correction.
            GX_Position3f32(v[0], v[1], -v[3]);
        } else {
            const float w = v[3];
            const float inv_w = (w != 0.0f) ? 1.0f / w : 0.0f;
            // Depth conventions do not line up. z_is_from_0_to_1() made gfx_pc
            // give us 0 at the near plane and 1 at the far plane; GX wants -1
            // near and 0 far. Hence the -1 shift rather than a negation.
            //
            // This shift was replaced by a negation once, on the strength of a
            // depth reading taken while the hardware projection above was
            // feeding a second, incompatible convention into the same buffer.
            // The reading was of a corrupted buffer and the conclusion was
            // wrong: it inverted the sort order, which is what put the white of
            // Mario's eyes in front of his pupils and Bowser under the floor.
            GX_Position3f32(v[0] * inv_w, v[1] * inv_w, (v[2] * inv_w) - 1.0f);
        }

#if defined(GFX_GX_DEBUG_CC)
        // Which combiner translation case this surface takes.
        //   red   = uses a texture
        //   green = the two-stage general form, the only one that can go wrong
        //           through intermediate clamping
        //   blue  = an operand is SHADER_TEXEL0A (texture alpha used as colour)
        GX_Color4u8(cur_shader->cc.used_textures[0] ? 230 : 30,
                    cur_shader->dbg_case == 3 ? 230 : 30,
                    cur_shader->dbg_uses_texel0a ? 230 : 30, 255);
#elif defined(GFX_GX_DEBUG_INPUTS)
        // How many combiner inputs actually vary across this batch. The backend
        // can only route one through the rasteriser; any second one is frozen at
        // the first vertex's value, which would show up as flat or blotchy
        // shading.
        //   blue = 0 varying, green = 1, yellow = 2, red = 3 or more
        switch (dbg_num_varying) {
            case 0:  GX_Color4u8( 40,  40, 255, 255); break;
            case 1:  GX_Color4u8( 40, 220,  40, 255); break;
            case 2:  GX_Color4u8(240, 240,  40, 255); break;
            default: GX_Color4u8(255,  40,  40, 255); break;
        }
#elif defined(GFX_GX_DEBUG_ZSTATE)
        // Encodes the depth state gfx_pc asked for, so a surface that occludes
        // the scene can be read straight off the screen:
        //   red   = depth test enabled
        //   green = depth writes enabled
        //   blue  = decal mode
        GX_Color4u8(gx_state.depth_test ? 255 : 40,
                    (gx_state.depth_test && gx_state.depth_mask) ? 255 : 40,
                    gx_state.zmode_decal ? 255 : 40, 255);
#elif defined(GFX_GX_DEBUG_DEPTH)
        // Greyscale depth: black at the near plane, white at the far plane.
        // Whatever survives the depth test shows its own depth, which is how to
        // find a surface that is occluding the scene from the wrong distance.
        {
            const float w = v[3];
            const float zn = (w != 0.0f) ? (v[2] / w) : 0.0f;
            const u8 g = float_to_u8(zn);
            GX_Color4u8(g, g, g, 255);
        }
#elif defined(GFX_GX_DEBUG_BATCH)
        // One hue per draw call. If the whole screen is a single colour, only
        // one batch is covering it and the rest of the scene is not being
        // submitted; many hues mean the geometry is there.
        {
            const uint32_t h = dbg_batch_id * 2654435761u;
            GX_Color4u8((u8) (h >> 24) | 0x40, (u8) (h >> 16) | 0x40, (u8) (h >> 8) | 0x40, 255);
        }
#elif defined(GFX_GX_DEBUG_UV)
        if (tex_in_buffer) {
            const float fu = v[tex_off] - floorf(v[tex_off]);
            const float fv = v[tex_off + 1] - floorf(v[tex_off + 1]);
            GX_Color4u8(float_to_u8(fu), float_to_u8(fv), 0, 255);
        } else {
            GX_Color4u8(0, 0, 255, 255);   // untextured geometry shows blue
        }
#elif defined(GFX_OGC_BRINGUP_DEBUG)
        // False colour, one hue per triangle. Until textures exist every
        // surface comes out white, which hides whether real geometry is being
        // submitted at all; this makes the shapes obvious.
        {
            const uint32_t h = (uint32_t) (i / 3) * 2654435761u;
            GX_Color4u8((u8) (h >> 24), (u8) (h >> 16), (u8) (h >> 8), 255);
        }
#else
        if (has_input && varying >= 0) {
            const float *c = v + input_off + varying * input_size;
            GX_Color4u8(float_to_u8(c[0]), float_to_u8(c[1]), float_to_u8(c[2]),
                        cc->opt_alpha ? float_to_u8(c[3]) : 255);
        } else {
            GX_Color4u8(255, 255, 255, 255);
        }
#endif

        if (submit_tex) {
            GX_TexCoord2f32(v[tex_off], v[tex_off + 1]);
        }
    }
    GX_End();
}

// -- lifecycle ---------------------------------------------------------------

static void gfx_gx_init(void) {
    Mtx identity;

    // gfx_pc already transformed everything, so the GX transform unit is set to
    // identity and does nothing but the projection.
    guMtxIdentity(identity);
    GX_LoadPosMtxImm(identity, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);

    // The projection is chosen per batch from here on (see the block above
    // gfx_gx_setup_perspective); start from the orthographic one.
    cur_proj_mode = PROJ_NONE;
    gfx_gx_load_ortho();

    // No hardware lighting: gfx_pc has applied the N64 lighting model on the
    // CPU, so the vertex colour must reach the TEV untouched.
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);

    GX_SetCullMode(GX_CULL_NONE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZCompLoc(GX_TRUE);

    gx_state.depth_test = false;
    gx_state.depth_mask = false;
    gx_state.zmode_decal = false;
    gx_state.use_alpha = false;
    gx_state.initialised = false;
    gfx_gx_apply_zmode();
    gfx_gx_set_use_alpha(false);
    gx_state.initialised = true;
}

static void gfx_gx_on_resize(void) {
}

#ifdef GFX_OGC_BRINGUP_DEBUG
// Build with -DGFX_OGC_BRINGUP_DEBUG to draw a fixed triangle in NDC before the
// game draws anything. It isolates the parts of the pipeline that have nothing
// to do with gfx_pc: projection, viewport, vertex format, TEV, EFB->XFB copy.
// If this shows up but the game does not, the fault is upstream of GX.
static void gfx_gx_draw_test_triangle(void) {
    GX_SetNumTexGens(0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetNumTevStages(1);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
        GX_Position3f32(-0.8f, -0.8f, -0.5f); GX_Color4u8(255,   0,   0, 255);
        GX_Position3f32( 0.8f, -0.8f, -0.5f); GX_Color4u8(  0, 255,   0, 255);
        GX_Position3f32( 0.0f,  0.8f, -0.5f); GX_Color4u8(  0,   0, 255, 255);
    GX_End();

    // The game re-applies whatever it needs, but the state cache has to be told
    // that the GX state no longer matches what it believes.
    gfx_gx_apply_zmode();
    gx_state.initialised = false;
    gfx_gx_set_use_alpha(gx_state.use_alpha);
    gx_state.initialised = true;
}
#endif

static void gfx_gx_start_frame(void) {
    // The state cache describes the GX pipeline, which survives across frames,
    // so only the caches the GP itself keeps need invalidating.
    GX_InvVtxCache();
    GX_InvalidateTexAll();
    cur_shader = NULL;

    // Resynchronise the state cache with the hardware.
    //
    // The EFB -> XFB copy in gfx_ogc.c has to force GX_SetZMode(GX_TRUE, ...,
    // GX_TRUE) and GX_SetColorUpdate(GX_TRUE), otherwise GX_CopyDisp does not
    // clear the EFB. That happens after the last draw of a frame and behind
    // this file's back, so the cache below still describes the state the game
    // asked for while the hardware has been moved elsewhere.
    //
    // The cache only emits on a change, so if the first draw of the next frame
    // happens to match what the cache already believes, nothing is emitted and
    // the draw runs with the copy's depth state. SM64's full-screen background
    // rectangle asks for no depth test; left writing depth at zn = 0, the near
    // plane, it stamps the whole buffer and the 3D scene behind it loses every
    // comparison -- only the surfaces that draw without testing, the HUD and
    // PRESS START, survive. Whether the first draw matches varies frame to
    // frame, so the scene and the HUD take turns.
    //
    // start_frame runs at the top of every gfx_run, i.e. right after any copy.
    gfx_gx_apply_zmode();
    GX_SetColorUpdate(GX_TRUE);
}

static void gfx_gx_end_frame(void) {
#ifdef GFX_OGC_BRINGUP_DEBUG
    // Drawn last, so it lands on top of the full-screen black rectangle SM64
    // paints as its background -- putting it in start_frame simply hid it, which
    // cost a debugging cycle.
    gfx_gx_draw_test_triangle();
#endif
    GX_DrawDone();
}

static void gfx_gx_finish_render(void) {
}

struct GfxRenderingAPI gfx_gx_api = {
    gfx_gx_z_is_from_0_to_1,
    gfx_gx_unload_shader,
    gfx_gx_load_shader,
    gfx_gx_create_and_load_new_shader,
    gfx_gx_lookup_shader,
    gfx_gx_shader_get_info,
    gfx_gx_new_texture,
    gfx_gx_select_texture,
    gfx_gx_upload_texture,
    gfx_gx_set_sampler_parameters,
    gfx_gx_set_depth_test,
    gfx_gx_set_depth_mask,
    gfx_gx_set_zmode_decal,
    gfx_gx_set_viewport,
    gfx_gx_set_scissor,
    gfx_gx_set_use_alpha,
    gfx_gx_draw_triangles,
    gfx_gx_init,
    gfx_gx_on_resize,
    gfx_gx_start_frame,
    gfx_gx_end_frame,
    gfx_gx_finish_render
};

#endif // ENABLE_GX
