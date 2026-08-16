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
#define GFX_GX_TEXTURES_IMPLEMENTED 1

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

static void gfx_gx_apply_zmode(void) {
    // A decal (Mario's shadow, footprints) must test against the surface it
    // sits on but never write depth, otherwise it fights with it.
    if (gx_state.zmode_decal) {
        GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    } else {
        GX_SetZMode(gx_state.depth_test ? GX_TRUE : GX_FALSE,
                    GX_LEQUAL,
                    gx_state.depth_mask ? GX_TRUE : GX_FALSE);
    }
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

        if (!cc->opt_alpha) {
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

    GX_SetNumTevStages(prg->num_stages ? prg->num_stages : 1);
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
    const size_t input_size = cc->opt_alpha ? 4 : 3;
    const size_t num_verts = buf_vbo_num_tris * 3;

    // Which combiner input actually varies across the batch? Only CC_SHADE does
    // in practice; PRIM, ENV and LOD are constant for the whole draw. The
    // varying one goes through the rasteriser, the constants into TEV registers,
    // because GX only has two per-vertex colour channels against four inputs.
    int varying = -1;
    for (int j = 0; j < cc->num_inputs && varying < 0; j++) {
        const float *ref = buf_vbo + input_off + j * input_size;
        for (size_t i = 1; i < num_verts; i++) {
            const float *cur = buf_vbo + i * stride + input_off + j * input_size;
            if (memcmp(ref, cur, input_size * sizeof(float)) != 0) {
                varying = j;
                break;
            }
        }
    }

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

    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, (u16) (buf_vbo_num_tris * 3));
    for (size_t i = 0; i < buf_vbo_num_tris * 3; i++) {
        const float *v = buf_vbo + i * stride;

        // gfx_pc hands us clip space; the perspective divide happens here for
        // now, with an identity orthographic projection downstream. That makes
        // interpolation affine, which is invisible while nothing is textured
        // but will have to be revisited in STORY-009.
        const float w = v[3];
        const float inv_w = (w != 0.0f) ? 1.0f / w : 0.0f;
        // Depth conventions do not line up. z_is_from_0_to_1() made gfx_pc give
        // us 0 at the near plane and 1 at the far plane; GX wants -1 near and 0
        // far. Hence the -1 shift rather than a negation: negating maps near to
        // far and lets the background win every depth test, which shows up as a
        // uniform full-screen fill.
        GX_Position3f32(v[0] * inv_w, v[1] * inv_w, (v[2] * inv_w) - 1.0f);

#ifdef GFX_OGC_BRINGUP_DEBUG
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
    Mtx44 proj;
    Mtx identity;

    // gfx_pc already transformed everything, so the GX transform unit is set to
    // identity and does nothing but the (currently trivial) projection.
    guMtxIdentity(identity);
    GX_LoadPosMtxImm(identity, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);

    // Not guMtxIdentity(): that takes a Mtx (3x4) and would leave the fourth
    // row of a Mtx44 uninitialised. Both types decay to f32(*)[4], so the
    // compiler says nothing about the mismatch.
    memset(proj, 0, sizeof(proj));
    proj[0][0] = proj[1][1] = proj[2][2] = proj[3][3] = 1.0f;
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);

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
