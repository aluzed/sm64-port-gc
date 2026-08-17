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

// How far a ZMODE_DEC decal is pulled towards the viewer, in NDC, where the
// depth range is [-1, 0] and one level of the 24-bit buffer is about 6e-8.
//
// It only has to break a tie. A shadow is coplanar with its ground, so the two
// depths differ by interpolation rounding, not by any real distance; 1e-4 is
// some sixteen hundred levels, comfortably above that noise and still under a
// world unit at the distance the camera normally sits from the floor.
//
// The tension to watch, if this ever needs changing: a constant bias in NDC is
// a small world-space offset near the camera and a large one far away, because
// the depth range is compressed with distance. Too small and shadows shimmer
// again; too large and a shadow lifts off a slope seen edge-on, or punches
// through a thin floor. Override with -DGFX_GX_DECAL_BIAS=... to try a value.
#ifndef GFX_GX_DECAL_BIAS
#define GFX_GX_DECAL_BIAS 0.0001f
#endif

// Distance fog. Set to 0 to drop the fog stage entirely, which is the A/B to
// run whenever a scene looks washed out: an over-applied fog and a missing
// texture are hard to tell apart by eye.
#ifndef GFX_GX_FOG
#define GFX_GX_FOG 1
#endif

// Alpha dither ("noise"). Set to 0 to drop the stage and let the shaders that
// ask for it draw at full alpha, which is a smooth fade rather than a stippled
// one -- the degradation STORY-010 allows if this ever has to be switched off.
#ifndef GFX_GX_NOISE
#define GFX_GX_NOISE 1
#endif

// Side of the square dither texture, in texels. The pattern repeats across the
// screen every this many cells; 64 is small enough to stay in texture cache and
// large enough that the tiling is not readable as a grid.
#define GFX_GX_NOISE_SIZE 64

// The virtual raster the reference backend quantises to: it computes the dither
// on floor(gl_FragCoord.xy * (240 / window_height)), so one dither cell is one
// N64 pixel whatever the real resolution. Reproducing that here is what keeps
// the stipple the same size at 240p, 480i and 480p.
#define GFX_GX_NOISE_RASTER_H 240.0f
#define GFX_GX_NOISE_RASTER_W 320.0f

// Calibration for the projection-path view: force every batch onto a marked
// path, so the screen must come out entirely magenta. Any surface that keeps
// its texture under this flag is a surface the marking cannot reach -- and an
// unmarked surface in the real view would then mean nothing. Run it once before
// trusting a negative result.
#ifdef GFX_GX_DEBUG_PROJ_TINT_SELFTEST
#define GFX_GX_DEBUG_PROJ_TINT 1
#endif

// The debug views that replace the whole TEV chain with a single PASSCLR stage
// and paint the vertex colour directly. They bypass the shader, so anything the
// shader would have set up -- the fog channel, for one -- must be skipped at
// submission time too, or the vertex descriptor stops matching the pipeline.
#if defined(GFX_GX_DEBUG_UV) || defined(GFX_GX_DEBUG_BATCH) || defined(GFX_GX_DEBUG_DEPTH) \
    || defined(GFX_GX_DEBUG_ZSTATE) || defined(GFX_GX_DEBUG_INPUTS) \
    || defined(GFX_GX_DEBUG_CC)
#define GFX_GX_FLAT_DEBUG 1
#else
#define GFX_GX_FLAT_DEBUG 0
#endif

// Worst case is: one stage to stash texel1, two for the general colour form,
// and the alpha form padded to match. Eight leaves room for STORY-010.
#define MAX_TEV_STAGES 8

// GX has three colour/output registers usable as TEV inputs beside TEVPREV,
// which we keep as the accumulator.
#define NUM_TEV_REGS 3

struct TevStage {
    u8 tex_coord, tex_map;
    u8 chan;                         // GX_COLOR0A0 (shade) or GX_COLOR1A1 (fog)
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
    // A decal -- a shadow, a footprint, a painting surface -- must test against
    // the surface it sits on and never write depth, or it fights with it.
    //
    // Not writing is only half of it. The decal is *coplanar* with its host, so
    // the two interpolate to depths that differ only by rounding: GX_LEQUAL
    // then passes on some pixels and fails on others, which is z-fighting, and
    // on a large flat ground it reads as a shimmering, partly missing shadow.
    // The reference backend adds glPolygonOffset(-2, -2) for exactly this. The
    // bias itself is applied in draw_triangles and in the projection.
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

// The dither texture and the frame counter that animates it. Declared here
// rather than with the texture pool below because the TEV setup binds them, and
// that runs earlier in the file.
static GXTexObj noise_tex_obj;
static bool noise_tex_ready;
static uint32_t noise_frame;

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
        s->chan = GX_COLOR0A0;
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
        s->chan = GX_COLOR0A0;

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

    // Fog, as one final stage.
    //
    // The reference backend computes mix(colour, fog.rgb, fog.a) on the colour
    // only, leaving alpha alone. A TEV stage is exactly that interpolation:
    //   out = d + (1 - c) * a + c * b
    // so with d = 0, a = CPREV, b = the fog colour and c = the fog factor it is
    // the mix, in one stage and with no arithmetic of our own.
    //
    // GX has no fog input, but it has a second rasterised colour channel. The
    // factor is per vertex -- gfx_pc computes it from the N64's fog_mul and
    // fog_offset and stores it in the vertex alpha, forcing shade alpha to 1.0
    // to free the slot -- so the colour and the factor travel together as the
    // RGBA of GX_COLOR1A1, read here as RASC and RASA.
    //
    // GX_SetFog was rejected: it derives the factor from screen Z on its own
    // curve, which would not match what gfx_pc computed.
    if (cc->opt_fog && GFX_GX_FOG && stage < MAX_TEV_STAGES) {
        struct TevStage *s = &prg->stages[stage++];
        s->tex_coord = GX_TEXCOORDNULL;
        s->tex_map = GX_TEXMAP_NULL;
        s->chan = GX_COLOR1A1;
        s->col_a = GX_CC_CPREV; s->col_b = GX_CC_RASC; s->col_c = GX_CC_RASA;
        s->col_d = GX_CC_ZERO;
        s->col_op = GX_TEV_ADD;
        s->col_clamp = GX_TRUE;
        s->out_reg = GX_TEVPREV;

        // Alpha passes through untouched, or stays pinned at 1.0 when the
        // combiner has no alpha channel.
        s->alp_a = GX_CA_ZERO; s->alp_b = GX_CA_ZERO; s->alp_c = GX_CA_ZERO;
        s->alp_op = GX_TEV_ADD;
        s->alp_clamp = GX_TRUE;
        if (!cc->opt_alpha || !GFX_GX_TEXTURES_IMPLEMENTED) {
            s->alp_d = GX_CA_KONST;
            s->alpha_konst_one = true;
        } else {
            s->alp_d = GX_CA_APREV;
            s->alpha_konst_one = false;
        }
    }

    // Noise, as one more final stage -- after fog, which is the order the
    // reference fragment shader uses (combiner, texture edge, fog, noise).
    //
    // The reference computes a per-pixel coin flip and multiplies alpha by it:
    //   texel.a *= floor(random(screen_cell, frame) + 0.5)
    // so alpha is either kept or zeroed, half the cells each. That is a screen
    // door, not a soft grain, and it is what makes SM64's fades dissolve rather
    // than cross-fade.
    //
    // TEV cannot generate randomness, so the flip comes from a texture of
    // pre-rolled zeroes and ones sampled in screen space (see the texgen in
    // gfx_gx_emit_tev). The stage itself is the multiply:
    //   out = d + (1 - c) * a + c * b, with a = 0, b = APREV, c = the sampled
    // flip, d = 0, which is APREV * flip.
    //
    // Colour passes through untouched: the reference only ever multiplies the
    // alpha channel.
    if (cc->opt_noise && cc->opt_alpha && GFX_GX_NOISE && !GFX_GX_FLAT_DEBUG
        && stage < MAX_TEV_STAGES) {
        struct TevStage *s = &prg->stages[stage++];
        // GX requires the texgens in use to be 0..n-1 with no gap, so the
        // dither takes coordinate 1 only when the shader's own texture has
        // already taken 0.
        s->tex_coord = (use_tex0 || use_tex1) ? GX_TEXCOORD1 : GX_TEXCOORD0;
        s->tex_map = GX_TEXMAP2;
        s->chan = GX_COLORNULL;
        s->col_a = GX_CC_ZERO; s->col_b = GX_CC_ZERO; s->col_c = GX_CC_ZERO;
        s->col_d = GX_CC_CPREV;
        s->col_op = GX_TEV_ADD;
        s->col_clamp = GX_TRUE;
        s->out_reg = GX_TEVPREV;

        s->alp_a = GX_CA_ZERO; s->alp_b = GX_CA_APREV; s->alp_c = GX_CA_TEXA;
        s->alp_d = GX_CA_ZERO;
        s->alp_op = GX_TEV_ADD;
        s->alp_clamp = GX_TRUE;
        s->alpha_konst_one = false;
    }

    prg->num_stages = (uint8_t) stage;
}

static void gfx_gx_emit_tev(const struct ShaderProgram *prg) {
#if GFX_GX_FLAT_DEBUG
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

    // The dither needs a coordinate of its own, generated from the position
    // rather than supplied per vertex: it has to land in screen space, so that
    // the pattern stays put on screen while geometry moves through it. The
    // matrix is loaded per batch in draw_triangles, because which one is
    // correct depends on the projection that batch chose.
    const bool use_noise = prg->cc.opt_noise && prg->cc.opt_alpha && GFX_GX_NOISE;

    u8 ngen = 0;
    if (use_tex) {
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        ngen++;
    }
    if (use_noise) {
        GX_SetTexCoordGen(GX_TEXCOORD0 + ngen, GX_TG_MTX3x4, GX_TG_POS, GX_TEXMTX0);
        ngen++;
        if (noise_tex_ready) {
            GX_LoadTexObj(&noise_tex_obj, GX_TEXMAP2);
        }
    }
    GX_SetNumTexGens(ngen);

    // A second rasterised channel exists only to carry the fog colour and
    // factor. Enabling it unconditionally would cost a channel on every shader.
    if (prg->cc.opt_fog && GFX_GX_FOG) {
        GX_SetNumChans(2);
        GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, 0,
                       GX_DF_NONE, GX_AF_NONE);
    } else {
        GX_SetNumChans(1);
    }

    for (int i = 0; i < prg->num_stages; i++) {
        const struct TevStage *s = &prg->stages[i];
        const u8 st = GX_TEVSTAGE0 + i;

        // Stages that use no texture must be given the null coordinate and null
        // map, or they sample whatever is left in texture memory.
        GX_SetTevOrder(st, s->tex_coord, s->tex_map, s->chan);

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

// Bound whenever the texture a shader asks for has no valid object. Skipping
// the load instead leaves the *previous* texture bound, so the surface silently
// wears whatever was drawn before it -- a bug that reads as "the texture is
// missing" while looking like anything at all.
//
// White is the graceful fallback: a shader multiplying by white renders as if
// untextured. -DGFX_GX_DEBUG_TEXFAIL makes it magenta instead, which tells
// "the texture object was never valid" apart from "it is valid but wrong" in
// one run.
static GXTexObj fallback_tex_obj;
static bool fallback_tex_ready;

static void gfx_gx_init_fallback_texture(void) {
    // One 4x4 GX_TF_RGBA8 tile: 32 bytes of AR pairs, then 32 of GB.
    static u8 texels[64] __attribute__((aligned(32)));
#ifdef GFX_GX_DEBUG_TEXFAIL
    const u8 r = 255, g = 0, b = 255;
#else
    const u8 r = 255, g = 255, b = 255;
#endif
    for (int i = 0; i < 16; i++) {
        texels[i * 2]          = 255;   // A
        texels[i * 2 + 1]      = r;
        texels[32 + i * 2]     = g;
        texels[32 + i * 2 + 1] = b;
    }
    DCFlushRange(texels, sizeof(texels));
    GX_InitTexObj(&fallback_tex_obj, texels, 4, 4, GX_TF_RGBA8,
                  GX_REPEAT, GX_REPEAT, GX_FALSE);
    GX_InitTexObjFilterMode(&fallback_tex_obj, GX_NEAR, GX_NEAR);
    fallback_tex_ready = true;
}

static void gfx_gx_init_noise_texture(void) {
    // GX_TF_I8: one byte per texel, the intensity replicated into RGB *and*
    // alpha, so the value the TEV reads as TEXA is simply the byte. 4 KB.
    static u8 texels[GFX_GX_NOISE_SIZE * GFX_GX_NOISE_SIZE] __attribute__((aligned(32)));

    // I8 is tiled in 8x4 blocks, and every other texture in this backend has to
    // be swizzled into that layout on upload. This one does not, and the reason
    // is worth stating because the absence looks like an omission: the content
    // is random, so any permutation of it is equally random.
    //
    // A plain LCG, run once at boot. The pattern only has to be uncorrelated to
    // the eye. Bit 16 is the coin flip -- the low bits of an LCG are the weak
    // ones -- reproducing the reference's floor(random + 0.5): half the cells
    // keep their alpha, half lose it.
    uint32_t s = 0x1234567u;
    for (size_t i = 0; i < sizeof(texels); i++) {
        s = s * 1664525u + 1013904223u;
        texels[i] = (s & 0x10000u) ? 255 : 0;
    }

    DCFlushRange(texels, sizeof(texels));
    GX_InitTexObj(&noise_tex_obj, texels, GFX_GX_NOISE_SIZE, GFX_GX_NOISE_SIZE,
                  GX_TF_I8, GX_REPEAT, GX_REPEAT, GX_FALSE);
    // Nearest, always. A filtered dither is a grey haze, not a screen door.
    GX_InitTexObjFilterMode(&noise_tex_obj, GX_NEAR, GX_NEAR);
    noise_tex_ready = true;
}

// Screen-space coordinates for the dither texture, as a texgen matrix.
//
// GX generates the coordinate from the position *before* the projection matrix,
// so which matrix is correct depends on what the batch submitted -- and the two
// paths in this backend submit different spaces. GX_TG_MTX3x4 emits (s, t, q)
// and divides, which is what lets one mechanism serve both.
static void gfx_gx_load_noise_texmtx(bool perspective) {
    // One dither cell per pixel of the reference's 240-line virtual raster, and
    // NDC spans [-1, 1], hence the half.
    const float ku = 0.5f * GFX_GX_NOISE_RASTER_W / (float) GFX_GX_NOISE_SIZE;
    const float kv = 0.5f * GFX_GX_NOISE_RASTER_H / (float) GFX_GX_NOISE_SIZE;

    // Reseed per frame by jumping a whole number of texels, not by sliding:
    // a smooth shift would read as the dither crawling across the screen, where
    // the reference redraws it from scratch every frame. The texture repeats,
    // so any offset is in range.
    const float ou = (float) ((noise_frame * 37u) % GFX_GX_NOISE_SIZE)
                     / (float) GFX_GX_NOISE_SIZE;
    const float ov = (float) ((noise_frame * 97u + 13u) % GFX_GX_NOISE_SIZE)
                     / (float) GFX_GX_NOISE_SIZE;
    const float cu = ku + ou;
    const float cv = kv + ov;

    Mtx m;
    memset(m, 0, sizeof(m));
    m[0][0] = ku;
    m[1][1] = kv;

    if (perspective) {
        // View space, with z carrying -w (see gfx_gx_load_persp). Dividing by
        // q = w turns x into NDC -- and the constant term has to ride z as well,
        // or it would be divided too and the pattern would shrink with distance.
        m[0][2] = -cu;
        m[1][2] = -cv;
        m[2][2] = -1.0f;
    } else {
        // The CPU path already divided, so the position is NDC and no further
        // divide is wanted: q = 1.
        m[0][3] = cu;
        m[1][3] = cv;
        m[2][3] = 1.0f;
    }

    GX_LoadTexMtxImm(m, GX_TEXMTX0, GX_MTX3x4);
}

static void gfx_gx_bind_texture(const struct GXTexture *t, u8 map) {
    if (t->obj_valid) {
        GX_LoadTexObj((GXTexObj *) &t->obj, map);
    } else if (fallback_tex_ready) {
        GX_LoadTexObj(&fallback_tex_obj, map);
    }
}

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

// Which route through the projection choice this batch took, for
// -DGFX_GX_DEBUG_PROJ_TINT. Recorded even when the view is compiled out, so the
// branches that set it stay one shape whether or not the measurement is
// enabled -- they are exactly the branches under suspicion, and an #ifdef
// through the middle of them would let the measured build diverge from the
// shipped one. Declared outside GFX_GX_HW_PERSP because the CPU path sets it
// too, and -DGFX_GX_HW_PERSP=0 is a supported A/B.
enum ProjPath {
    PROJ_PATH_CPU,          // divided on the CPU; no near-plane clipping
    PROJ_PATH_FIT,          // fitted from this batch, residual held
    PROJ_PATH_BORROW,       // could not fit; used the last fit that held
    PROJ_PATH_BAD_FIT,      // fit failed and nothing to borrow; used it anyway
    PROJ_PATH_CONST_DEPTH,  // no spread and nothing to borrow; one depth
};
static enum ProjPath dbg_proj_path;

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

// The last fit that held. The relation z/w = p + q/w describes gfx_pc's
// projection matrix, not the batch: every batch drawn through the same matrix
// recovers the same pair, and a batch only fails to recover it when it is too
// ill-conditioned to measure -- not because its depth is different.
//
// So a batch that cannot fit anything itself can borrow. That matters because
// the alternative is a constant depth, and a constant depth is not a small
// error: it pins a whole polygon to one plane, so a floor stops occluding what
// is behind it and geometry shows through the ground. Reported from hardware as
// polygons appearing at the junction of two map faces when the camera turns.
static float last_fit_p, last_fit_q;
static bool have_last_fit;

// Whether p + q/w reproduces the batch's own depth, over every vertex in front
// of the eye. Guards the borrow above: if gfx_pc has changed projection since
// the pair was recorded, this is what notices.
static bool gfx_gx_fit_holds(const float *buf, size_t stride, size_t nverts,
                             float p, float q, float tol) {
    for (size_t i = 0; i < nverts; i++) {
        const float w = buf[i * stride + 3];
        if (w <= 1e-6f) {
            continue;
        }
        const float iw = 1.0f / w;
        if (fabsf((p + q * iw) - buf[i * stride + 2] * iw) > tol) {
            return false;
        }
    }
    return true;
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
    bool crosses_near_plane = false;

    // Vertices at or behind the eye are exactly the ones the hardware path
    // exists for -- dividing them on the CPU is what produces the giant
    // triangles. They cannot contribute to the fit, but their presence must not
    // disqualify the batch.
    for (size_t i = 0; i < nverts; i++) {
        const float w = buf[i * stride + 3];
        if (w <= 1e-6f) {
            crosses_near_plane = true;
            continue;
        }
        usable++;
        const float iw = 1.0f / w;
        if (iw < iw_lo) { iw_lo = iw; lo = i; }
        if (iw > iw_hi) { iw_hi = iw; hi = i; }
    }

#ifdef GFX_GX_DEBUG_OLD_NEARPLANE
    // Restores the behaviour from before "Clip near-plane batches on the GP":
    // crossing the near plane earned a batch no exemption, so a bad fit sent it
    // to the CPU divide like any other. Exists to answer one question -- did
    // that commit introduce the triangles, or were they already there -- and
    // nothing else. Delete it once the question is closed.
    crosses_near_plane = false;
#endif

    // Fewer than three vertices in front of the eye: nothing to fit a projection
    // from. That was made a flat refusal, and the refusal is what produced the
    // triangles -- confirmed from the projection-path view, where they came out
    // magenta, the colour of the CPU divide.
    //
    // The reasoning behind sending near-plane batches to the GP applies here
    // more strongly than anywhere else, not less: this is the batch that is
    // *most* behind the eye. Refusing it hands it to a CPU path that cannot
    // clip, and that path now collapses a vertex with w <= 0 onto (0, 0) -- the
    // centre of the screen -- so the triangle is dragged into the middle of the
    // image instead of being flung out of frame and discarded. That collapse is
    // bounded and correct in isolation; it is only visible because the batch
    // should never have reached it.
    //
    // Being unable to *measure* the projection is not being unable to *use* one.
    // Borrow it, exactly as the two branches below already do, and let the GP
    // clip. With no usable vertex at all the whole batch is behind the eye and
    // the GP discards it, so any consistent projection will do.
    if (usable < 3) {
        if (!crosses_near_plane) {
            return false;   // genuinely degenerate, and safely affine
        }
        const float bias = gx_state.zmode_decal ? GFX_GX_DECAL_BIAS : 0.0f;
        if (have_last_fit) {
            gfx_gx_load_persp(1.0f - last_fit_p + bias, last_fit_q);
            dbg_proj_path = PROJ_PATH_BORROW;
            return true;
        }
        gfx_gx_load_persp(1.0f - (usable > 0 ? buf[lo * stride + 2] * iw_lo : 0.0f) + bias,
                          0.0f);
        dbg_proj_path = PROJ_PATH_CONST_DEPTH;
        return true;
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
    //
    // A batch that crosses the near plane is the exception, and it is not
    // optional. Handing it to the CPU divide means dividing by a w at or below
    // zero: the vertex is flung across the screen and drags its triangle into a
    // corner as a huge flat wedge. That is the defect reported from a real
    // console when the camera turns and a polygon edge reaches the screen
    // boundary. Such a batch takes the hardware path whatever the fit looks
    // like, because only the GP's clipper can cut it correctly.
    //
    // What it is drawn with is a separate question, and the first answer was
    // wrong. A constant-depth projection keeps x and y exact, but "the depth is
    // approximate" understates what it does: the whole batch lands on one
    // plane. The batches that reach here are large floor and wall polygons seen
    // nearly edge-on -- precisely the ones that must occlude -- so the trade
    // bought a clipped wedge and sold the depth buffer. Borrow the projection
    // instead: it is the same matrix, and a batch that cannot measure it is
    // still drawn by it.
    if (iw_hi - iw_lo < 0.01f * iw_hi) {
        if (!crosses_near_plane) {
            return false;
        }
        const float bias = gx_state.zmode_decal ? GFX_GX_DECAL_BIAS : 0.0f;
        // No spread means no depth span to judge against, so the check is a
        // loose one: it asks whether the borrowed pair is the same projection,
        // not whether it is accurate to the last bit.
        const float borrow_tol =
            fmaxf(1e-3f, 1e-5f * fmaxf(fabsf(last_fit_p) + fabsf(last_fit_q) * iw_hi, 1.0f));
        if (have_last_fit
            && gfx_gx_fit_holds(buf, stride, nverts, last_fit_p, last_fit_q, borrow_tol)) {
            gfx_gx_load_persp(1.0f - last_fit_p + bias, last_fit_q);
            dbg_proj_path = PROJ_PATH_BORROW;
            return true;
        }
        // Nothing to borrow -- no perspective batch has fitted yet, or the one
        // that did belongs to another projection. Constant depth is then the
        // last resort it was always meant to be.
        gfx_gx_load_persp(1.0f - (buf[lo * stride + 2] * iw_lo) + bias, 0.0f);
        dbg_proj_path = PROJ_PATH_CONST_DEPTH;
        return true;
    }

    const float zn_lo = buf[lo * stride + 2] * iw_lo;
    const float zn_hi = buf[hi * stride + 2] * iw_hi;

    const float q = (zn_hi - zn_lo) / (iw_hi - iw_lo);
    const float p = zn_lo - q * iw_lo;

    // Validate on every vertex, not on a sample.
    //
    // This used to check the single vertex furthest from both anchors. Two
    // points define the line and a third confirms it only if the relation is
    // known to be affine -- which is exactly what is in question. A batch whose
    // depth is not affine in 1/w, because gfx_pc changed projection inside it,
    // slips through a one-point check and is then drawn through a projection
    // fitted to something else. The mesh comes out as a fan of stretched
    // slivers, which on screen reads as a crosshatch of thin lines, and
    // whatever leaves the [-1, 0] range is clipped away outright -- a floor
    // that disappears.
    //
    // The residual is judged against the batch's own depth spread, not an
    // absolute epsilon: an object occupying 2% of the depth range would accept
    // a fit 5% wrong across itself under a 1e-3 absolute tolerance, enough to
    // scramble which of its own polygons is in front.
    //
    // The floor under that tolerance has to track the magnitude of the terms,
    // not be a fixed epsilon. p is computed as zn_lo - q * iw_lo, a difference
    // of two quantities that can both be large, so single precision loses
    // digits exactly where the spread is smallest. A fixed 1e-6 floor rejected
    // batches that were perfectly affine and merely noisy, sending them to the
    // CPU divide -- whose affine interpolation is what makes a texture stretch
    // across a large polygon.
    const float zn_span = fabsf(zn_hi - zn_lo);
    const float scale = fabsf(p) + fabsf(q) * iw_hi;
    const float tol = fmaxf(0.02f * zn_span, 1e-5f * fmaxf(scale, 1.0f));

    // Rejecting a batch because the fit puts a vertex outside [0, 1] would buy
    // nothing: GX clips z_ndc outside [-1, 0] whichever path produced it, and
    // the CPU path writes zn - 1, so the same vertex is cut either way. All
    // that matters here is whether the fit is faithful.
    const bool fit_holds = gfx_gx_fit_holds(buf, stride, nverts, p, q, tol);

    const float bias = gx_state.zmode_decal ? GFX_GX_DECAL_BIAS : 0.0f;

    if (!fit_holds) {
        // Same exception as above: a batch crossing the near plane has to be
        // clipped by the GP whatever its depth looks like.
        if (!crosses_near_plane) {
            return false;
        }
        // The local fit is known wrong here -- that is what the residual just
        // said -- so prefer a pair that was measured on a batch that could be
        // measured, when it still describes this one.
        if (have_last_fit
            && gfx_gx_fit_holds(buf, stride, nverts, last_fit_p, last_fit_q, tol)) {
            gfx_gx_load_persp(1.0f - last_fit_p + bias, last_fit_q);
            dbg_proj_path = PROJ_PATH_BORROW;
            return true;
        }
        // Neither pair describes the batch. Draw it through the local fit
        // anyway: wrong depth on one batch is a sorting artefact, a CPU divide
        // by a negative w is a wedge across a quarter of the screen. Do not
        // record a fit that failed.
        gfx_gx_load_persp(1.0f - p + bias, q);
        dbg_proj_path = PROJ_PATH_BAD_FIT;
        return true;
    }

    // A fit that held is the projection gfx_pc is drawing with, so it is worth
    // keeping for the batches that cannot recover it themselves.
    last_fit_p = p;
    last_fit_q = q;
    have_last_fit = true;

    // GX gives z_ndc = -mt22 + mt23/w, and the CPU path writes z_ndc = zn - 1
    // with zn = p + q/w. Matching the two: mt22 = 1 - p, mt23 = q.
    //
    // Getting this pair wrong is what split the depth buffer between two
    // conventions; keep it derived from whatever the CPU path in
    // draw_triangles writes, never from the two independently.
    //
    // The decal bias follows the same rule. Subtracting b from z_ndc is
    // -(mt22 + b) + mt23/w, so it is one addition here and the two paths stay
    // in step. The previous attempt at a decal bias sidestepped this by forcing
    // decals onto the CPU divide, which cost them perspective-correct
    // interpolation for no reason.
    gfx_gx_load_persp(1.0f - p + bias, q);
    dbg_proj_path = PROJ_PATH_FIT;
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
    // The fog colour and factor ride the second colour channel; see the fog
    // stage in gfx_gx_build_tev. The attribute order below must match the order
    // the vertices are written in, POS, CLR0, CLR1, TEX0.
    const bool submit_fog = cc->opt_fog && GFX_GX_FOG && !GFX_GX_FLAT_DEBUG;
    if (submit_fog) {
        GX_SetVtxDesc(GX_VA_CLR1, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR1, GX_CLR_RGBA, GX_RGBA8, 0);
    }
    if (submit_tex) {
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    }

    if (cc->used_textures[0] && GFX_GX_TEXTURES_IMPLEMENTED) {
        struct GXTexture *t0 = &texture_pool[cur_tex_id[0]];
        gfx_gx_bind_texture(t0, GX_TEXMAP0);
    }
    if (cc->used_textures[1] && GFX_GX_TEXTURES_IMPLEMENTED) {
        struct GXTexture *t1 = &texture_pool[cur_tex_id[1]];
        gfx_gx_bind_texture(t1, GX_TEXMAP1);
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
        dbg_proj_path = PROJ_PATH_CPU;
    }

    // Has to follow the projection choice above, not precede it: the dither's
    // texgen reads the submitted position, and which space that is depends on
    // the path this batch just took.
    if (cc->opt_noise && cc->opt_alpha && GFX_GX_NOISE && !GFX_GX_FLAT_DEBUG) {
        gfx_gx_load_noise_texmtx(hw_persp);
    }

#ifdef GFX_GX_DEBUG_PROJ_TINT_SELFTEST
    dbg_proj_path = PROJ_PATH_CPU;   // everything marked; see the flag's comment
#endif

#ifdef GFX_GX_DEBUG_PROJ_TINT
    // Mark flagged batches in a way no shader can route around.
    //
    // Placement is the whole point, and the first attempt got it wrong: this
    // block sat before gfx_gx_setup_perspective, so it read the *previous*
    // batch's path while the vertex colour below read the current one. The two
    // halves of the instrument disagreed. The calibration flag caught it --
    // grass came out flat white, meaning the TEV override had fired on a batch
    // the colour switch considered nominal. It must run after the projection
    // choice, like the dither above and for the same reason.
    //
    // The mark replaces the chain rather than tinting the vertex colour, which
    // was the attempt before that: only one combiner input is routed through
    // the rasteriser (see gfx_gx_build_tev), so a shader with no varying input
    // never reads the channel and a tint went nowhere. One PASSCLR stage is
    // unconditional. Texgens and the vertex format are left untouched so the
    // stream still matches what GX expects; the stage samples no texture.
    if (dbg_proj_path != PROJ_PATH_FIT) {
        GX_SetNumTevStages(1);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        // Force the shader's own chain to be rebuilt for the next batch.
        cur_shader->built_for_varying = -128;
    }
#endif

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
            // Collapse a vertex at or behind the eye to the centre of the near
            // plane instead of dividing by its w. A negative w mirrors the
            // vertex through the origin and flings it off screen, dragging its
            // triangle into a corner as a large flat wedge -- the defect seen
            // on hardware when the camera turns and a polygon edge reaches the
            // screen boundary.
            //
            // setup_perspective now sends every batch that crosses the near
            // plane to the GP, which clips it properly, so this is only reached
            // when fewer than three vertices are in front of the eye and there
            // is nothing to fit. Degenerate either way; bounded is better.
#ifdef GFX_GX_DEBUG_OLD_NEARPLANE
            // The other half of the same revert: the CPU path used to mirror a
            // vertex with w < 0 through the origin rather than collapse it.
            const float inv_w = (w != 0.0f) ? 1.0f / w : 0.0f;
#else
            const float inv_w = (w > 1e-6f) ? 1.0f / w : 0.0f;
#endif
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
            //
            // Nearer is more negative here, so a decal is biased by
            // subtracting. Clamped, because a decal on geometry already at the
            // near plane would otherwise be pushed past it and clipped away.
            float z = (v[2] * inv_w) - 1.0f;
            if (gx_state.zmode_decal) {
                z -= GFX_GX_DECAL_BIAS;
                if (z < -1.0f) {
                    z = -1.0f;
                }
            }
            GX_Position3f32(v[0] * inv_w, v[1] * inv_w, z);
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
        {
            u8 cr = 255, cg = 255, cb = 255, ca = 255;
            if (has_input && varying >= 0) {
                const float *c = v + input_off + varying * input_size;
                cr = float_to_u8(c[0]);
                cg = float_to_u8(c[1]);
                cb = float_to_u8(c[2]);
                ca = cc->opt_alpha ? float_to_u8(c[3]) : 255;
            }
#ifdef GFX_GX_DEBUG_PROJ_TINT
            // Which route through gfx_gx_setup_perspective this batch took.
            //
            // Only the paths under suspicion are marked, and they are marked
            // flat -- the TEV override above sees to that. The nominal path is
            // left byte-for-byte as a normal build draws it, textures and all,
            // so the scene stays navigable and a marked surface means exactly
            // one thing. In particular, if the defect keeps its texture, it
            // took the good path and the projection fit is exonerated.
            //   magenta = divided on the CPU: no near-plane clipping, affine
            //             interpolation. Produces both the wedge and the smear
            //   yellow  = could not fit, borrowed the last fit that held
            //   red     = the fit failed and there was nothing to borrow
            //   cyan    = no spread and nothing to borrow: one depth for the
            //             whole batch
            // Alpha is forced opaque on marked batches: a blend must not be
            // able to hide the answer.
            switch (dbg_proj_path) {
                case PROJ_PATH_FIT:     break;   // untouched, renders normally
                case PROJ_PATH_CPU:     cr = 255; cg =   0; cb = 255; ca = 255; break;
                case PROJ_PATH_BORROW:  cr = 255; cg = 255; cb =   0; ca = 255; break;
                case PROJ_PATH_BAD_FIT: cr = 255; cg =   0; cb =   0; ca = 255; break;
                default:                cr =   0; cg = 255; cb = 255; ca = 255; break;
            }
#endif
            GX_Color4u8(cr, cg, cb, ca);
        }
#endif

        if (submit_fog) {
            // rgb = fog colour, a = the fog factor gfx_pc computed per vertex.
            const float *f = v + 4 + (tex_in_buffer ? 2 : 0);
            GX_Color4u8(float_to_u8(f[0]), float_to_u8(f[1]), float_to_u8(f[2]),
                        float_to_u8(f[3]));
        }

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
    // The fog stage reads this one; it is only switched on for shaders that
    // carry opt_fog, but the control has to be configured once.
    GX_SetChanCtrl(GX_COLOR1A1, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);

    gfx_gx_init_fallback_texture();
    gfx_gx_init_noise_texture();

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

    // Advances the dither pattern. The reference feeds the frame counter into
    // its hash, so the flip is redrawn every frame; here the frame number picks
    // a fresh offset into the texture instead.
    noise_frame++;

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
