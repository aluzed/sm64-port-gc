#ifdef TARGET_OGC

// Window-manager backend for GameCube and Wii (libogc).
//
// There is no window and no event loop on a console, so most of
// GfxWindowManagerAPI collapses into no-ops. What is left is the part that
// actually matters: bringing up the video interface, allocating the external
// framebuffers, and pacing the game loop on the vertical retrace.
//
// This file also owns the GX bring-up and the EFB -> XFB copy. That was first
// done so the port could boot before gfx_gx.c existed, and it stayed: those
// settings are a function of the video mode (fbWidth, efbHeight, aa,
// sample_pattern, vfilter), not of the renderer, and keeping them here is what
// lets ENABLE_GFX_DUMMY builds still come up on screen. gfx_gx.c owns the
// render state -- TEV, textures, depth, blending, vertex submission.

#include <gccore.h>
#include <ogcsys.h>
#include <ogc/lwp_watchdog.h> // gettime, ticks_to_microsecs

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_ogc.h"
#include "gfx_window_manager_api.h"

#define OGC_FIFO_SIZE (256 * 1024)

static GXRModeObj *rmode;
static void *xfb[2];
static int cur_xfb;
static void *gp_fifo;

static uint64_t start_ticks;

// -- frame pacing ------------------------------------------------------------
//
// 60 Hz modes present every two retraces, which is exactly 29.97 fps and
// exactly what the N64 did. 50 Hz cannot express 30 fps that way, so it gets
// time-based pacing instead: wait whole retraces until 1/30 s has elapsed,
// averaging 30 game frames per second with a regular 2-2-1 retrace pattern.
//
// Forcing PAL60 was considered and rejected: it overrides the console's own
// setting, and on a TV that cannot do 60 Hz the result is no picture at all.
#define VSYNCS_PER_FRAME 2
#define GAME_FRAME_USEC  33333

static bool fifty_hz;
static u64 next_frame_ticks;

static bool video_is_50hz(void) {
    const u32 tv = VIDEO_GetCurrentTvMode();
    return tv == VI_PAL || tv == VI_DEBUG_PAL;
}
static volatile bool should_quit;
static volatile bool should_power_off;


// -- shutdown ---------------------------------------------------------------

void gfx_ogc_request_quit(bool power_off) {
    if (power_off) {
        should_power_off = true;
    }
    should_quit = true;
}

// These run in interrupt context: they may only raise a flag. Touching the
// filesystem or GX from here locks the console in a way that is very hard to
// diagnose.
static void on_reset_pressed(u32 irq, void *ctx) {
    (void) irq;
    (void) ctx;
    gfx_ogc_request_quit(false);
}

#ifdef TARGET_WII
// The GameCube has no soft power button, so libogc only exposes this on Wii.
static void on_power_pressed(void) {
    gfx_ogc_request_quit(true);
}
#endif

static void gfx_ogc_shutdown(void) {
    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
    VIDEO_WaitVSync();

#ifdef TARGET_WII
    if (should_power_off) {
        SYS_ResetSystem(SYS_POWEROFF, 0, 0);
    }
#endif
    // On Wii the loader installs a return stub, so exit() goes back to the
    // Homebrew Channel. SYS_ResetSystem is the fallback when it does not.
    exit(0);
}

// -- init -------------------------------------------------------------------

static void gfx_ogc_init_video(void) {
    VIDEO_Init();

    // Honours the console's own settings: PAL/NTSC, 50/60 Hz, 480p when a
    // component cable is present. Picking a mode by hand is STORY-011.
    rmode = VIDEO_GetPreferredMode(NULL);

    // MEM_K0_TO_K1 is mandatory: the XFB must be accessed uncached, otherwise
    // the display shows stale or corrupted data.
    xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    cur_xfb = 0;

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb[cur_xfb]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) {
        VIDEO_WaitVSync();
    }
}

// ---- GX bring-up: video-mode dependent, shared with ENABLE_GFX_DUMMY -------
static void gfx_ogc_init_gx(void) {
#ifdef GFX_OGC_BRINGUP_DEBUG
    // A black clear makes "the renderer draws nothing" indistinguishable from
    // "the EFB->XFB copy is broken". Build with -DGFX_OGC_BRINGUP_DEBUG to tell
    // the two apart: if the screen turns blue, everything downstream of drawing
    // works and the fault is in the draw path.
    GXColor background = { 0, 0, 0x80, 0xFF };
#else
    GXColor background = { 0, 0, 0, 0xFF };
#endif

    gp_fifo = memalign(32, OGC_FIFO_SIZE);
    memset(gp_fifo, 0, OGC_FIFO_SIZE);
    GX_Init(gp_fifo, OGC_FIFO_SIZE);

    GX_SetCopyClear(background, GX_MAX_Z24);

    GX_SetViewport(0.0f, 0.0f, rmode->fbWidth, rmode->efbHeight, 0.0f, 1.0f);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);

    u32 xfb_height = GX_SetDispCopyYScale(
        GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight));
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, xfb_height);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering,
                    (rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(rmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);

    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_CopyDisp(xfb[cur_xfb], GX_TRUE);
}

static void gfx_ogc_copy_to_xfb(void) {
    GX_DrawDone();
    GX_SetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);
    GX_CopyDisp(xfb[cur_xfb], GX_TRUE);
    GX_DrawDone();
}
// ---- end of GX bring-up ----------------------------------------------------

static void gfx_ogc_init(const char *game_name, bool start_in_fullscreen) {
    (void) game_name;
    (void) start_in_fullscreen;

    gfx_ogc_init_video();
    gfx_ogc_init_gx();

    SYS_SetResetCallback(on_reset_pressed);
#ifdef TARGET_WII
    SYS_SetPowerCallback(on_power_pressed);
#endif

    start_ticks = gettime();
    fifty_hz = video_is_50hz();
    next_frame_ticks = start_ticks + microsecs_to_ticks(GAME_FRAME_USEC);
}

// -- GfxWindowManagerAPI -----------------------------------------------------

static void gfx_ogc_set_keyboard_callbacks(bool (*on_key_down)(int scancode),
                                           bool (*on_key_up)(int scancode),
                                           void (*on_all_keys_up)(void)) {
    (void) on_key_down;
    (void) on_key_up;
    (void) on_all_keys_up;
}

static void gfx_ogc_set_fullscreen_changed_callback(void (*on_fullscreen_changed)(bool)) {
    (void) on_fullscreen_changed;
}

static void gfx_ogc_set_fullscreen(bool enable) {
    (void) enable;
}

static void gfx_ogc_main_loop(void (*run_one_game_iter)(void)) {
    // The loop is paced by VIDEO_WaitVSync() in swap_buffers_end, so there is
    // no timing logic here.
    while (!should_quit) {
        run_one_game_iter();
    }
    gfx_ogc_shutdown();
}

static void gfx_ogc_get_dimensions(uint32_t *width, uint32_t *height) {
    *width = rmode->fbWidth;
    *height = rmode->efbHeight;
}

static void gfx_ogc_handle_events(void) {
    // Controllers are polled by the ControllerAPI backend.
}

static bool gfx_ogc_start_frame(void) {
    return true;
}

static void gfx_ogc_swap_buffers_begin(void) {
    // Deliberately empty. The EFB -> XFB copy used to live here, which was
    // wrong: gfx_pc calls this once per gfx_run(), i.e. once per display list,
    // while GX_CopyDisp(..., GX_TRUE) also *clears* the EFB. When a frame is
    // built from more than one display list -- which is what SM64's intro does,
    // drawing the background and the Goddard Mario head separately -- each copy
    // wiped what the previous one had accumulated, so the head appeared only on
    // the frames where it happened to be last.
    //
    // The copy belongs in swap_buffers_end, which runs exactly once per
    // presented frame.
}

// SM64 is a 30 fps game: the N64 produces one frame every two VI retraces, and
// the PC backends cap themselves at 1000/30 ms (sync_framerate_with_timer in
// gfx_sdl2.c). Presenting on every retrace runs the whole game -- logic, physics
// and audio -- at double speed. Measured on Dolphin before this was added:
// 59.94 fps instead of 29.97.
//
// Two retraces is exactly right on a 60 Hz mode, and exactly what the N64 did.
// On a 50 Hz PAL mode it gives 25 fps, i.e. the 17% slowdown the original PAL
// release suffered -- and it is not only a speed problem: the audio backend
// feeds the DMA one block per game frame, so at 25 fps it starves continuously.
// See the pacing block near the top of the file for what 50 Hz does instead.

static void gfx_ogc_swap_buffers_end(void) {
    gfx_ogc_copy_to_xfb();

    VIDEO_SetNextFramebuffer(xfb[cur_xfb]);
    VIDEO_Flush();

    if (!fifty_hz) {
        for (int i = 0; i < VSYNCS_PER_FRAME; i++) {
            VIDEO_WaitVSync();
        }
    } else {
        // Always burn at least one retrace, both to bound the loop and to keep
        // the flip synchronised with the scan-out.
        VIDEO_WaitVSync();
        while (gettime() < next_frame_ticks) {
            VIDEO_WaitVSync();
        }
        next_frame_ticks += microsecs_to_ticks(GAME_FRAME_USEC);

        // After a long stall -- a level load, say -- the deadline is far in the
        // past and catching up would run the game fast. Resynchronise instead.
        const u64 now = gettime();
        if (now > next_frame_ticks) {
            next_frame_ticks = now + microsecs_to_ticks(GAME_FRAME_USEC);
        }
    }

    cur_xfb ^= 1;
}

static double gfx_ogc_get_time(void) {
    // Subtract first: ticks_to_microsecs on a raw uptime loses precision once
    // it is converted to double.
    return (double) ticks_to_microsecs(gettime() - start_ticks) / 1000000.0;
}

uint64_t gfx_ogc_get_ticks(void) {
    return gettime();
}

uint32_t gfx_ogc_framebuffer_width(void) {
    return rmode->fbWidth;
}

uint32_t gfx_ogc_framebuffer_height(void) {
    return rmode->efbHeight;
}

struct GfxWindowManagerAPI gfx_ogc_wm_api = {
    gfx_ogc_init,
    gfx_ogc_set_keyboard_callbacks,
    gfx_ogc_set_fullscreen_changed_callback,
    gfx_ogc_set_fullscreen,
    gfx_ogc_main_loop,
    gfx_ogc_get_dimensions,
    gfx_ogc_handle_events,
    gfx_ogc_start_frame,
    gfx_ogc_swap_buffers_begin,
    gfx_ogc_swap_buffers_end,
    gfx_ogc_get_time
};

#endif // TARGET_OGC
