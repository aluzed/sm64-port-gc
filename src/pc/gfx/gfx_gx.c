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

struct ShaderProgram {
    uint32_t shader_id;
    struct CCFeatures cc;
    uint8_t num_floats;   // per vertex, as laid out by gfx_sp_tri1 in gfx_pc.c
    bool used_textures[2];
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

static void gfx_gx_load_shader(struct ShaderProgram *new_prg) {
    if (new_prg == cur_shader) {
        return;
    }
    cur_shader = new_prg;

    const struct CCFeatures *cc = &new_prg->cc;
    const bool use_tex = (cc->used_textures[0] || cc->used_textures[1])
                         && GFX_GX_TEXTURES_IMPLEMENTED;

    // PROVISIONAL. Faithfully translating the N64 colour combiner into TEV
    // stages is STORY-007; until then every shader gets the one configuration
    // that is right most of the time: texel modulated by the vertex colour.
    if (use_tex) {
        GX_SetNumTexGens(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    } else {
        GX_SetNumTexGens(0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    }
    GX_SetNumTevStages(1);
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

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    if (submit_tex) {
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    }

    if (submit_tex) {
        struct GXTexture *t0 = &texture_pool[cur_tex_id[0]];
        if (t0->obj_valid) {
            GX_LoadTexObj(&t0->obj, GX_TEXMAP0);
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
        if (has_input) {
            const float *c = v + input_off;
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
